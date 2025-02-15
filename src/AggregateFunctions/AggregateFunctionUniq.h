#pragma once

#include <city.h>
#include <type_traits>

#include <base/bit_cast.h>

#include <IO/WriteHelpers.h>
#include <IO/ReadHelpers.h>

#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/DataTypeTuple.h>

#include <Interpreters/AggregationCommon.h>

#include <Common/HashTable/Hash.h>
#include <Common/HashTable/HashSet.h>
#include <Common/HyperLogLogWithSmallSetOptimization.h>
#include <Common/CombinedCardinalityEstimator.h>
#include <Common/typeid_cast.h>
#include <Common/assert_cast.h>

#include <AggregateFunctions/UniquesHashSet.h>
#include <AggregateFunctions/IAggregateFunction.h>
#include <AggregateFunctions/ThetaSketchData.h>
#include <AggregateFunctions/UniqVariadicHash.h>


namespace DB
{
struct Settings;

/// uniq

struct AggregateFunctionUniqUniquesHashSetData
{
    using Set = UniquesHashSet<DefaultHash<UInt64>>;
    Set set;

    static String getName() { return "uniq"; }
};

/// For a function that takes multiple arguments. Such a function pre-hashes them in advance, so TrivialHash is used here.
struct AggregateFunctionUniqUniquesHashSetDataForVariadic
{
    using Set = UniquesHashSet<TrivialHash>;
    Set set;

    static String getName() { return "uniq"; }
};


/// uniqHLL12

template <typename T>
struct AggregateFunctionUniqHLL12Data
{
    using Set = HyperLogLogWithSmallSetOptimization<T, 16, 12>;
    Set set;

    static String getName() { return "uniqHLL12"; }
};

template <>
struct AggregateFunctionUniqHLL12Data<String>
{
    using Set = HyperLogLogWithSmallSetOptimization<UInt64, 16, 12>;
    Set set;

    static String getName() { return "uniqHLL12"; }
};

template <>
struct AggregateFunctionUniqHLL12Data<UUID>
{
    using Set = HyperLogLogWithSmallSetOptimization<UInt64, 16, 12>;
    Set set;

    static String getName() { return "uniqHLL12"; }
};

struct AggregateFunctionUniqHLL12DataForVariadic
{
    using Set = HyperLogLogWithSmallSetOptimization<UInt64, 16, 12, TrivialHash>;
    Set set;

    static String getName() { return "uniqHLL12"; }
};


/// uniqExact

template <typename T>
struct AggregateFunctionUniqExactData
{
    using Key = T;

    /// When creating, the hash table must be small.
    using Set = HashSet<
        Key,
        HashCRC32<Key>,
        HashTableGrower<4>,
        HashTableAllocatorWithStackMemory<sizeof(Key) * (1 << 4)>>;

    Set set;

    static String getName() { return "uniqExact"; }
};

/// For rows, we put the SipHash values (128 bits) into the hash table.
template <>
struct AggregateFunctionUniqExactData<String>
{
    using Key = UInt128;

    /// When creating, the hash table must be small.
    using Set = HashSet<
        Key,
        UInt128TrivialHash,
        HashTableGrower<3>,
        HashTableAllocatorWithStackMemory<sizeof(Key) * (1 << 3)>>;

    Set set;

    static String getName() { return "uniqExact"; }
};


/// uniqTheta
#if USE_DATASKETCHES

struct AggregateFunctionUniqThetaData
{
    using Set = ThetaSketchData<UInt64>;
    Set set;

    static String getName() { return "uniqTheta"; }
};

#endif

namespace detail
{

/** Hash function for uniq.
  */
template <typename T> struct AggregateFunctionUniqTraits
{
    static UInt64 hash(T x)
    {
        if constexpr (std::is_same_v<T, Float32> || std::is_same_v<T, Float64>)
        {
            return bit_cast<UInt64>(x);
        }
        else if constexpr (sizeof(T) <= sizeof(UInt64))
        {
            return x;
        }
        else
            return DefaultHash64<T>(x);
    }
};


/** The structure for the delegation work to add one element to the `uniq` aggregate functions.
  * Used for partial specialization to add strings.
  */
template <typename T, typename Data>
struct OneAdder
{
    static void ALWAYS_INLINE add(Data & data, const IColumn & column, size_t row_num)
    {
        if constexpr (std::is_same_v<Data, AggregateFunctionUniqUniquesHashSetData>
            || std::is_same_v<Data, AggregateFunctionUniqHLL12Data<T>>)
        {
            if constexpr (!std::is_same_v<T, String>)
            {
                using ValueType = typename decltype(data.set)::value_type;
                const auto & value = assert_cast<const ColumnVector<T> &>(column).getElement(row_num);
                data.set.insert(static_cast<ValueType>(AggregateFunctionUniqTraits<T>::hash(value)));
            }
            else
            {
                StringRef value = column.getDataAt(row_num);
                data.set.insert(CityHash_v1_0_2::CityHash64(value.data, value.size));
            }
        }
        else if constexpr (std::is_same_v<Data, AggregateFunctionUniqExactData<T>>)
        {
            if constexpr (!std::is_same_v<T, String>)
            {
                data.set.insert(assert_cast<const ColumnVector<T> &>(column).getData()[row_num]);
            }
            else
            {
                StringRef value = column.getDataAt(row_num);

                UInt128 key;
                SipHash hash;
                hash.update(value.data, value.size);
                hash.get128(key);

                data.set.insert(key);
            }
        }
#if USE_DATASKETCHES
        else if constexpr (std::is_same_v<Data, AggregateFunctionUniqThetaData>)
        {
            data.set.insertOriginal(column.getDataAt(row_num));
        }
#endif
    }
};

}


/// Calculates the number of different values approximately or exactly.
template <typename T, typename Data>
class AggregateFunctionUniq final : public IAggregateFunctionDataHelper<Data, AggregateFunctionUniq<T, Data>>
{
public:
    AggregateFunctionUniq(const DataTypes & argument_types_)
        : IAggregateFunctionDataHelper<Data, AggregateFunctionUniq<T, Data>>(argument_types_, {}) {}

    String getName() const override { return Data::getName(); }

    DataTypePtr getReturnType() const override
    {
        return std::make_shared<DataTypeUInt64>();
    }

    bool allocatesMemoryInArena() const override { return false; }

    /// ALWAYS_INLINE is required to have better code layout for uniqHLL12 function
    void ALWAYS_INLINE add(AggregateDataPtr __restrict place, const IColumn ** columns, size_t row_num, Arena *) const override
    {
        detail::OneAdder<T, Data>::add(this->data(place), *columns[0], row_num);
    }

    void addManyDefaults(
        AggregateDataPtr __restrict place,
        const IColumn ** columns,
        size_t /*length*/,
        Arena * /*arena*/) const override
    {
        detail::OneAdder<T, Data>::add(this->data(place), *columns[0], 0);
    }

    void merge(AggregateDataPtr __restrict place, ConstAggregateDataPtr rhs, Arena *) const override
    {
        this->data(place).set.merge(this->data(rhs).set);
    }

    void serialize(ConstAggregateDataPtr __restrict place, WriteBuffer & buf, std::optional<size_t> /* version */) const override
    {
        this->data(place).set.write(buf);
    }

    void deserialize(AggregateDataPtr __restrict place, ReadBuffer & buf, std::optional<size_t> /* version */, Arena *) const override
    {
        this->data(place).set.read(buf);
    }

    void insertResultInto(AggregateDataPtr __restrict place, IColumn & to, Arena *) const override
    {
        assert_cast<ColumnUInt64 &>(to).getData().push_back(this->data(place).set.size());
    }
};


/** For multiple arguments. To compute, hashes them.
  * You can pass multiple arguments as is; You can also pass one argument - a tuple.
  * But (for the possibility of efficient implementation), you can not pass several arguments, among which there are tuples.
  */
template <typename Data, bool is_exact, bool argument_is_tuple>
class AggregateFunctionUniqVariadic final : public IAggregateFunctionDataHelper<Data, AggregateFunctionUniqVariadic<Data, is_exact, argument_is_tuple>>
{
private:
    size_t num_args = 0;

public:
    AggregateFunctionUniqVariadic(const DataTypes & arguments)
        : IAggregateFunctionDataHelper<Data, AggregateFunctionUniqVariadic<Data, is_exact, argument_is_tuple>>(arguments, {})
    {
        if (argument_is_tuple)
            num_args = typeid_cast<const DataTypeTuple &>(*arguments[0]).getElements().size();
        else
            num_args = arguments.size();
    }

    String getName() const override { return Data::getName(); }

    DataTypePtr getReturnType() const override
    {
        return std::make_shared<DataTypeUInt64>();
    }

    bool allocatesMemoryInArena() const override { return false; }

    void add(AggregateDataPtr __restrict place, const IColumn ** columns, size_t row_num, Arena *) const override
    {
        this->data(place).set.insert(typename Data::Set::value_type(
            UniqVariadicHash<is_exact, argument_is_tuple>::apply(num_args, columns, row_num)));
    }

    void merge(AggregateDataPtr __restrict place, ConstAggregateDataPtr rhs, Arena *) const override
    {
        this->data(place).set.merge(this->data(rhs).set);
    }

    void serialize(ConstAggregateDataPtr __restrict place, WriteBuffer & buf, std::optional<size_t> /* version */) const override
    {
        this->data(place).set.write(buf);
    }

    void deserialize(AggregateDataPtr __restrict place, ReadBuffer & buf, std::optional<size_t> /* version */, Arena *) const override
    {
        this->data(place).set.read(buf);
    }

    void insertResultInto(AggregateDataPtr __restrict place, IColumn & to, Arena *) const override
    {
        assert_cast<ColumnUInt64 &>(to).getData().push_back(this->data(place).set.size());
    }
};

}
