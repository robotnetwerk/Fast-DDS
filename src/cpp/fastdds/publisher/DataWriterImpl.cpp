// Copyright 2019, 2020 Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/*
 * DataWriterImpl.cpp
 *
 */
#include <fastdds/publisher/DataWriterImpl.hpp>

#include <functional>
#include <iostream>

#include <fastdds/config.h>
#include <fastdds/core/condition/StatusConditionImpl.hpp>
#include <fastdds/core/policy/ParameterSerializer.hpp>
#include <fastdds/core/policy/QosPolicyUtils.hpp>
#include <fastdds/dds/core/ReturnCode.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/log/Log.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/PublisherListener.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastdds/domain/DomainParticipantImpl.hpp>
#include <fastdds/publisher/filtering/DataWriterFilteredChangePool.hpp>
#include <fastdds/publisher/PublisherImpl.hpp>
#include <fastdds/rtps/attributes/TopicAttributes.h>
#include <fastdds/rtps/participant/RTPSParticipant.h>
#include <fastdds/rtps/resources/ResourceEvent.h>
#include <fastdds/rtps/RTPSDomain.h>
#include <fastdds/rtps/writer/RTPSWriter.h>
#include <fastdds/rtps/writer/StatefulWriter.h>

#include <rtps/builtin/liveliness/WLP.h>
#include <rtps/DataSharing/DataSharingPayloadPool.hpp>
#include <rtps/history/CacheChangePool.h>
#include <rtps/history/TopicPayloadPoolRegistry.hpp>
#include <rtps/participant/RTPSParticipantImpl.h>
#include <rtps/RTPSDomainImpl.hpp>
#include <rtps/resources/TimedEvent.h>
#include <utils/TimeConversion.hpp>
#ifdef FASTDDS_STATISTICS
#include <statistics/fastdds/domain/DomainParticipantImpl.hpp>
#include <statistics/types/monitorservice_types.hpp>
#endif //FASTDDS_STATISTICS

using namespace eprosima::fastrtps;
using namespace eprosima::fastrtps::rtps;
using namespace std::chrono;

namespace eprosima {
namespace fastdds {
namespace dds {

static ChangeKind_t unregister_change_kind(
        bool dispose,
        const DataWriterQos& qos)
{
    if (dispose)
    {
        return NOT_ALIVE_DISPOSED;
    }

    return qos.writer_data_lifecycle().autodispose_unregistered_instances ?
           NOT_ALIVE_DISPOSED_UNREGISTERED : NOT_ALIVE_UNREGISTERED;
}

static bool qos_has_pull_mode_request(
        const DataWriterQos& qos)
{
    auto push_mode = PropertyPolicyHelper::find_property(qos.properties(), "fastdds.push_mode");
    return (nullptr != push_mode) && ("false" == *push_mode);
}

class DataWriterImpl::LoanCollection
{
public:

    explicit LoanCollection(
            const PoolConfig& config)
        : loans_(get_collection_limits(config))
    {
    }

    bool add_loan(
            void* data,
            PayloadInfo_t& payload)
    {
        static_cast<void>(data);
        assert(data == payload.payload.data + SerializedPayload_t::representation_header_size);
        return loans_.push_back(payload);
    }

    bool check_and_remove_loan(
            void* data,
            PayloadInfo_t& payload)
    {
        octet* payload_data = static_cast<octet*>(data) - SerializedPayload_t::representation_header_size;
        for (auto it = loans_.begin(); it != loans_.end(); ++it)
        {
            if (it->payload.data == payload_data)
            {
                payload = *it;
                loans_.erase(it);
                return true;
            }
        }
        return false;
    }

    bool is_empty() const
    {
        return loans_.empty();
    }

private:

    static ResourceLimitedContainerConfig get_collection_limits(
            const PoolConfig& config)
    {
        return
            {
                config.initial_size,
                config.maximum_size,
                config.initial_size == config.maximum_size ? 0u : 1u
            };
    }

    ResourceLimitedVector<PayloadInfo_t> loans_;

};

DataWriterImpl::DataWriterImpl(
        PublisherImpl* p,
        TypeSupport type,
        Topic* topic,
        const DataWriterQos& qos,
        DataWriterListener* listen,
        std::shared_ptr<fastrtps::rtps::IPayloadPool> payload_pool)
    : publisher_(p)
    , type_(type)
    , topic_(topic)
    , qos_(get_datawriter_qos_from_settings(qos))
    , listener_(listen)
    , history_(get_topic_attributes(qos_, *topic_, type_), type_->m_typeSize, qos_.endpoint().history_memory_policy,
            [this](
                const InstanceHandle_t& handle) -> void
            {
                if (nullptr != listener_)
                {
                    listener_->on_unacknowledged_sample_removed(user_datawriter_, handle);
                }
            })
#pragma warning (disable : 4355 )
    , writer_listener_(this)
    , deadline_duration_us_(qos_.deadline().period.to_ns() * 1e-3)
    , lifespan_duration_us_(qos_.lifespan().duration.to_ns() * 1e-3)
{
    EndpointAttributes endpoint_attributes;
    endpoint_attributes.endpointKind = WRITER;
    endpoint_attributes.topicKind = type_->m_isGetKeyDefined ? WITH_KEY : NO_KEY;
    endpoint_attributes.setEntityID(qos_.endpoint().entity_id);
    endpoint_attributes.setUserDefinedID(qos_.endpoint().user_defined_id);
    fastrtps::rtps::RTPSParticipantImpl::preprocess_endpoint_attributes<WRITER, 0x03, 0x02>(
        fastrtps::rtps::EntityId_t::unknown(),
        publisher_->get_participant_impl()->id_counter(), endpoint_attributes, guid_.entityId);
    guid_.guidPrefix = publisher_->get_participant_impl()->guid().guidPrefix;

    if (payload_pool != nullptr)
    {
        is_custom_payload_pool_ = true;
        payload_pool_ = payload_pool;
    }
}

DataWriterImpl::DataWriterImpl(
        PublisherImpl* p,
        TypeSupport type,
        Topic* topic,
        const DataWriterQos& qos,
        const fastrtps::rtps::EntityId_t& entity_id,
        DataWriterListener* listen)
    : publisher_(p)
    , type_(type)
    , topic_(topic)
    , qos_(get_datawriter_qos_from_settings(qos))
    , listener_(listen)
    , history_(get_topic_attributes(qos_, *topic_, type_), type_->m_typeSize, qos_.endpoint().history_memory_policy,
            [this](
                const InstanceHandle_t& handle) -> void
            {
                if (nullptr != listener_)
                {
                    listener_->on_unacknowledged_sample_removed(user_datawriter_, handle);
                }
            })
#pragma warning (disable : 4355 )
    , writer_listener_(this)
    , deadline_duration_us_(qos_.deadline().period.to_ns() * 1e-3)
    , lifespan_duration_us_(qos_.lifespan().duration.to_ns() * 1e-3)
{
    guid_ = { publisher_->get_participant_impl()->guid().guidPrefix, entity_id};
}

DataWriterQos DataWriterImpl::get_datawriter_qos_from_settings(
        const DataWriterQos& qos)
{
    DataWriterQos return_qos;
    if (&DATAWRITER_QOS_DEFAULT == &qos)
    {
        return_qos = publisher_->get_default_datawriter_qos();
    }
    else if (&DATAWRITER_QOS_USE_TOPIC_QOS == &qos)
    {
        return_qos = publisher_->get_default_datawriter_qos();
        publisher_->copy_from_topic_qos(return_qos, topic_->get_qos());
    }
    else
    {
        return_qos = qos;
    }

    return return_qos;
}

ReturnCode_t DataWriterImpl::enable()
{
    assert(writer_ == nullptr);

    WriterAttributes w_att;
    w_att.throughputController = qos_.throughput_controller();
    w_att.endpoint.durabilityKind = qos_.durability().durabilityKind();
    w_att.endpoint.endpointKind = WRITER;
    w_att.endpoint.reliabilityKind = qos_.reliability().kind == RELIABLE_RELIABILITY_QOS ? RELIABLE : BEST_EFFORT;
    w_att.endpoint.topicKind = type_->m_isGetKeyDefined ? WITH_KEY : NO_KEY;
    w_att.endpoint.multicastLocatorList = qos_.endpoint().multicast_locator_list;
    w_att.endpoint.unicastLocatorList = qos_.endpoint().unicast_locator_list;
    w_att.endpoint.remoteLocatorList = qos_.endpoint().remote_locator_list;
    w_att.endpoint.external_unicast_locators = qos_.endpoint().external_unicast_locators;
    w_att.endpoint.ignore_non_matching_locators = qos_.endpoint().ignore_non_matching_locators;
    w_att.mode = qos_.publish_mode().kind == SYNCHRONOUS_PUBLISH_MODE ? SYNCHRONOUS_WRITER : ASYNCHRONOUS_WRITER;
    w_att.flow_controller_name = qos_.publish_mode().flow_controller_name;
    w_att.endpoint.properties = qos_.properties();
    w_att.endpoint.ownershipKind = qos_.ownership().kind;
    w_att.endpoint.setEntityID(qos_.endpoint().entity_id);
    w_att.endpoint.setUserDefinedID(qos_.endpoint().user_defined_id);
    w_att.times = qos_.reliable_writer_qos().times;
    w_att.liveliness_kind = qos_.liveliness().kind;
    w_att.liveliness_lease_duration = qos_.liveliness().lease_duration;
    w_att.liveliness_announcement_period = qos_.liveliness().announcement_period;
    w_att.matched_readers_allocation = qos_.writer_resource_limits().matched_subscriber_allocation;
    w_att.disable_heartbeat_piggyback = qos_.reliable_writer_qos().disable_heartbeat_piggyback;

    // TODO(Ricardo) Remove in future
    // Insert topic_name and partitions
    Property property;
    property.name("topic_name");
    property.value(topic_->get_name().c_str());
    w_att.endpoint.properties.properties().push_back(std::move(property));

    std::string* endpoint_partitions = PropertyPolicyHelper::find_property(qos_.properties(), "partitions");

    if (endpoint_partitions)
    {
        property.name("partitions");
        property.value(*endpoint_partitions);
        w_att.endpoint.properties.properties().push_back(std::move(property));
    }
    else if (publisher_->get_qos().partition().names().size() > 0)
    {
        property.name("partitions");
        std::string partitions;
        bool is_first_partition = true;
        for (auto partition : publisher_->get_qos().partition().names())
        {
            partitions += (is_first_partition ? "" : ";") + partition;
            is_first_partition = false;
        }
        property.value(std::move(partitions));
        w_att.endpoint.properties.properties().push_back(std::move(property));
    }

    if (qos_.reliable_writer_qos().disable_positive_acks.enabled &&
            qos_.reliable_writer_qos().disable_positive_acks.duration != c_TimeInfinite)
    {
        w_att.disable_positive_acks = true;
        w_att.keep_duration = qos_.reliable_writer_qos().disable_positive_acks.duration;
    }

    ReturnCode_t ret_code = check_datasharing_compatible(w_att, is_data_sharing_compatible_);
    if (ret_code != RETCODE_OK)
    {
        return ret_code;
    }

    if (is_data_sharing_compatible_)
    {
        DataSharingQosPolicy datasharing(qos_.data_sharing());
        if (datasharing.domain_ids().empty())
        {
            datasharing.add_domain_id(utils::default_domain_id());
        }
        w_att.endpoint.set_data_sharing_configuration(datasharing);
    }
    else
    {
        DataSharingQosPolicy datasharing;
        datasharing.off();
        w_att.endpoint.set_data_sharing_configuration(datasharing);
    }

    bool filtering_enabled =
            qos_.liveliness().lease_duration.is_infinite() &&
            (0 < qos_.writer_resource_limits().reader_filters_allocation.maximum);
    if (filtering_enabled)
    {
        reader_filters_.reset(new ReaderFilterCollection(qos_.writer_resource_limits().reader_filters_allocation));
    }

    // Set Datawriter's DataRepresentationId taking into account the QoS.
    data_representation_ = qos_.representation().m_value.empty()
            || XCDR_DATA_REPRESENTATION == qos_.representation().m_value.at(0)
                    ? XCDR_DATA_REPRESENTATION : XCDR2_DATA_REPRESENTATION;

    auto change_pool = get_change_pool();
    if (!change_pool)
    {
        EPROSIMA_LOG_ERROR(DATA_WRITER, "Problem creating change pool for associated Writer");
        return RETCODE_ERROR;
    }

    auto pool = get_payload_pool();
    if (!pool)
    {
        EPROSIMA_LOG_ERROR(DATA_WRITER, "Problem creating payload pool for associated Writer");
        return RETCODE_ERROR;
    }

    RTPSWriter* writer =  RTPSDomainImpl::create_rtps_writer(
        publisher_->rtps_participant(),
        guid_.entityId,
        w_att,
        pool,
        change_pool,
        static_cast<WriterHistory*>(&history_),
        static_cast<WriterListener*>(&writer_listener_));

    if (writer == nullptr &&
            w_att.endpoint.data_sharing_configuration().kind() == DataSharingKind::AUTO)
    {
        EPROSIMA_LOG_INFO(DATA_WRITER, "Trying with a non-datasharing pool");
        release_payload_pool();
        is_data_sharing_compatible_ = false;
        DataSharingQosPolicy datasharing;
        datasharing.off();
        w_att.endpoint.set_data_sharing_configuration(datasharing);

        pool = get_payload_pool();
        if (!pool)
        {
            EPROSIMA_LOG_ERROR(DATA_WRITER, "Problem creating payload pool for associated Writer");
            return RETCODE_ERROR;
        }

        writer = RTPSDomainImpl::create_rtps_writer(
            publisher_->rtps_participant(),
            guid_.entityId,
            w_att,
            pool,
            change_pool,
            static_cast<WriterHistory*>(&history_),
            static_cast<WriterListener*>(&writer_listener_));
    }
    if (writer == nullptr)
    {
        release_payload_pool();
        EPROSIMA_LOG_ERROR(DATA_WRITER, "Problem creating associated Writer");
        return RETCODE_ERROR;
    }

    writer_ = writer;
    if (filtering_enabled)
    {
        writer_->reader_data_filter(this);
    }

    // In case it has been loaded from the persistence DB, rebuild instances on history
    history_.rebuild_instances();

    deadline_timer_ = new TimedEvent(publisher_->get_participant()->get_resource_event(),
                    [&]() -> bool
                    {
                        return deadline_missed();
                    },
                    qos_.deadline().period.to_ns() * 1e-6);

    lifespan_timer_ = new TimedEvent(publisher_->get_participant()->get_resource_event(),
                    [&]() -> bool
                    {
                        return lifespan_expired();
                    },
                    qos_.lifespan().duration.to_ns() * 1e-6);

    // In case it has been loaded from the persistence DB, expire old samples.
    if (qos_.lifespan().duration != c_TimeInfinite)
    {
        if (lifespan_expired())
        {
            lifespan_timer_->restart_timer();
        }
    }

    // REGISTER THE WRITER
    WriterQos wqos = qos_.get_writerqos(get_publisher()->get_qos(), topic_->get_qos());
    if (!is_data_sharing_compatible_)
    {
        wqos.data_sharing.off();
    }
    if (endpoint_partitions)
    {
        std::istringstream partition_string(*endpoint_partitions);
        std::string partition_name;
        wqos.m_partition.clear();

        while (std::getline(partition_string, partition_name, ';'))
        {
            wqos.m_partition.push_back(partition_name.c_str());
        }
    }
    publisher_->rtps_participant()->registerWriter(writer_, get_topic_attributes(qos_, *topic_, type_), wqos);

    return RETCODE_OK;
}

void DataWriterImpl::disable()
{
    set_listener(nullptr);
    if (writer_ != nullptr)
    {
        writer_->set_listener(nullptr);
    }
}

ReturnCode_t DataWriterImpl::check_delete_preconditions()
{
    if (loans_ && !loans_->is_empty())
    {
        return RETCODE_PRECONDITION_NOT_MET;
    }

    return RETCODE_OK;
}

DataWriterImpl::~DataWriterImpl()
{
    delete lifespan_timer_;
    delete deadline_timer_;

    if (writer_ != nullptr)
    {
        EPROSIMA_LOG_INFO(DATA_WRITER, guid().entityId << " in topic: " << type_->getName());
        RTPSDomain::removeRTPSWriter(writer_);
        release_payload_pool();
    }

    delete user_datawriter_;
}

ReturnCode_t DataWriterImpl::loan_sample(
        void*& sample,
        LoanInitializationKind initialization)
{
    // Block lowlevel writer
    auto max_blocking_time = steady_clock::now() +
            microseconds(rtps::TimeConv::Time_t2MicroSecondsInt64(qos_.reliability().max_blocking_time));

    // Type should be plain and have space for the representation header
    if (!type_->is_plain(data_representation_) || SerializedPayload_t::representation_header_size > type_->m_typeSize)
    {
        return RETCODE_ILLEGAL_OPERATION;
    }

    // Writer should be enabled
    if (nullptr == writer_)
    {
        return RETCODE_NOT_ENABLED;
    }

#if HAVE_STRICT_REALTIME
    std::unique_lock<RecursiveTimedMutex> lock(writer_->getMutex(), std::defer_lock);
    if (!lock.try_lock_until(max_blocking_time))
    {
        return RETCODE_TIMEOUT;
    }
#else
    static_cast<void>(max_blocking_time);
    std::lock_guard<RecursiveTimedMutex> lock(writer_->getMutex());
#endif // if HAVE_STRICT_REALTIME

    // Get one payload from the pool
    PayloadInfo_t payload;
    uint32_t size = type_->m_typeSize;
    if (!get_free_payload_from_pool([size]()
            {
                return size;
            }, payload))
    {
        return RETCODE_OUT_OF_RESOURCES;
    }

    // Leave payload state as if serialization has already been performed
    payload.payload.length = size;
    payload.payload.pos = size;
    payload.payload.data[1] = DEFAULT_ENCAPSULATION;
    payload.payload.encapsulation = DEFAULT_ENCAPSULATION;

    // Sample starts after representation header
    sample = payload.payload.data + SerializedPayload_t::representation_header_size;

    // Add to loans collection
    if (!add_loan(sample, payload))
    {
        sample = nullptr;
        return_payload_to_pool(payload);
        return RETCODE_OUT_OF_RESOURCES;
    }

    switch (initialization)
    {
        default:
            EPROSIMA_LOG_WARNING(DATA_WRITER, "Using wrong LoanInitializationKind value ("
                    << static_cast<int>(initialization) << "). Using default NO_LOAN_INITIALIZATION");
            break;

        case LoanInitializationKind::NO_LOAN_INITIALIZATION:
            break;

        case LoanInitializationKind::ZERO_LOAN_INITIALIZATION:
            if (SerializedPayload_t::representation_header_size < size)
            {
                size -= SerializedPayload_t::representation_header_size;
                memset(sample, 0, size);
            }
            break;

        case LoanInitializationKind::CONSTRUCTED_LOAN_INITIALIZATION:
            if (!type_->construct_sample(sample))
            {
                check_and_remove_loan(sample, payload);
                return_payload_to_pool(payload);
                sample = nullptr;
                return RETCODE_UNSUPPORTED;
            }
            break;
    }

    return RETCODE_OK;
}

ReturnCode_t DataWriterImpl::discard_loan(
        void*& sample)
{
    // Type should be plain and have space for the representation header
    if (!type_->is_plain(data_representation_) || SerializedPayload_t::representation_header_size > type_->m_typeSize)
    {
        return RETCODE_ILLEGAL_OPERATION;
    }

    // Writer should be enabled
    if (nullptr == writer_)
    {
        return RETCODE_NOT_ENABLED;
    }

    std::lock_guard<RecursiveTimedMutex> lock(writer_->getMutex());

    // Remove sample from loans collection
    PayloadInfo_t payload;
    if ((nullptr == sample) || !check_and_remove_loan(sample, payload))
    {
        return RETCODE_BAD_PARAMETER;
    }

    // Return payload to pool
    return_payload_to_pool(payload);
    sample = nullptr;

    return RETCODE_OK;
}

bool DataWriterImpl::write(
        void* data)
{
    if (writer_ == nullptr)
    {
        return false;
    }

    EPROSIMA_LOG_INFO(DATA_WRITER, "Writing new data");
    return RETCODE_OK == create_new_change(ALIVE, data);
}

bool DataWriterImpl::write(
        void* data,
        fastrtps::rtps::WriteParams& params)
{
    if (writer_ == nullptr)
    {
        return false;
    }

    EPROSIMA_LOG_INFO(DATA_WRITER, "Writing new data with WriteParams");
    return RETCODE_OK == create_new_change_with_params(ALIVE, data, params);
}

ReturnCode_t DataWriterImpl::check_write_preconditions(
        void* data,
        const InstanceHandle_t& handle,
        InstanceHandle_t& instance_handle)
{
    if (writer_ == nullptr)
    {
        return RETCODE_NOT_ENABLED;
    }

    if (type_.get()->m_isGetKeyDefined)
    {
        bool is_key_protected = false;
#if HAVE_SECURITY
        is_key_protected = writer_->getAttributes().security_attributes().is_key_protected;
#endif // if HAVE_SECURITY
        type_.get()->getKey(data, &instance_handle, is_key_protected);
    }

    //Check if the Handle is different from the special value HANDLE_NIL and
    //does not correspond with the instance referred by the data
    if (handle.isDefined() && handle != instance_handle)
    {
        return RETCODE_PRECONDITION_NOT_MET;
    }

    return RETCODE_OK;
}

ReturnCode_t DataWriterImpl::write(
        void* data,
        const InstanceHandle_t& handle)
{
    InstanceHandle_t instance_handle;
    ReturnCode_t ret = check_write_preconditions(data, handle, instance_handle);
    if (RETCODE_OK == ret)
    {
        EPROSIMA_LOG_INFO(DATA_WRITER, "Writing new data with Handle");
        WriteParams wparams;
        ret = create_new_change_with_params(ALIVE, data, wparams, instance_handle);
    }

    return ret;
}

ReturnCode_t DataWriterImpl::write_w_timestamp(
        void* data,
        const InstanceHandle_t& handle,
        const fastrtps::Time_t& timestamp)
{
    InstanceHandle_t instance_handle;
    ReturnCode_t ret = RETCODE_OK;
    if (timestamp.is_infinite() || timestamp.seconds < 0)
    {
        ret = RETCODE_BAD_PARAMETER;
    }

    if (RETCODE_OK == ret)
    {
        ret = check_write_preconditions(data, handle, instance_handle);
    }

    if (RETCODE_OK == ret)
    {
        EPROSIMA_LOG_INFO(DATA_WRITER, "Writing new data with Handle and timestamp");
        WriteParams wparams;
        wparams.source_timestamp(timestamp);
        ret = create_new_change_with_params(ALIVE, data, wparams, instance_handle);
    }

    return ret;
}

ReturnCode_t DataWriterImpl::check_instance_preconditions(
        void* data,
        const InstanceHandle_t& handle,
        InstanceHandle_t& instance_handle)
{
    if (nullptr == writer_)
    {
        return RETCODE_NOT_ENABLED;
    }

    if (nullptr == data)
    {
        EPROSIMA_LOG_ERROR(DATA_WRITER, "Data pointer not valid");
        return RETCODE_BAD_PARAMETER;
    }

    if (!type_->m_isGetKeyDefined)
    {
        EPROSIMA_LOG_ERROR(DATA_WRITER, "Topic is NO_KEY, operation not permitted");
        return RETCODE_PRECONDITION_NOT_MET;
    }

    instance_handle = handle;

#if defined(NDEBUG)
    if (!instance_handle.isDefined())
#endif // if !defined(NDEBUG)
    {
        bool is_key_protected = false;
#if HAVE_SECURITY
        is_key_protected = writer_->getAttributes().security_attributes().is_key_protected;
#endif // if HAVE_SECURITY
        type_->getKey(data, &instance_handle, is_key_protected);
    }

#if !defined(NDEBUG)
    if (handle.isDefined() && instance_handle != handle)
    {
        EPROSIMA_LOG_ERROR(DATA_WRITER, "handle differs from data's key.");
        return RETCODE_PRECONDITION_NOT_MET;
    }
#endif // if !defined(NDEBUG)

    return RETCODE_OK;
}

InstanceHandle_t DataWriterImpl::register_instance(
        void* key)
{
    /// Preconditions
    InstanceHandle_t instance_handle;
    if (RETCODE_OK != check_instance_preconditions(key, HANDLE_NIL, instance_handle))
    {
        return HANDLE_NIL;
    }

    WriteParams wparams;
    return do_register_instance(key, instance_handle, wparams);
}

InstanceHandle_t DataWriterImpl::register_instance_w_timestamp(
        void* key,
        const fastrtps::Time_t& timestamp)
{
    /// Preconditions
    InstanceHandle_t instance_handle;
    if (timestamp.is_infinite() || timestamp.seconds < 0 ||
            (RETCODE_OK != check_instance_preconditions(key, HANDLE_NIL, instance_handle)))
    {
        return HANDLE_NIL;
    }

    WriteParams wparams;
    wparams.source_timestamp(timestamp);
    return do_register_instance(key, instance_handle, wparams);
}

InstanceHandle_t DataWriterImpl::do_register_instance(
        void* key,
        const InstanceHandle_t instance_handle,
        WriteParams& wparams)
{
    // TODO(MiguelCompany): wparams should be used when propagating the register_instance operation to the DataReader.
    // See redmine issue #14494
    static_cast<void>(wparams);

    // Block lowlevel writer
    auto max_blocking_time = std::chrono::steady_clock::now() +
            std::chrono::microseconds(rtps::TimeConv::Time_t2MicroSecondsInt64(qos_.reliability().max_blocking_time));

#if HAVE_STRICT_REALTIME
    std::unique_lock<RecursiveTimedMutex> lock(writer_->getMutex(), std::defer_lock);
    if (lock.try_lock_until(max_blocking_time))
#else
    std::unique_lock<RecursiveTimedMutex> lock(writer_->getMutex());
#endif // if HAVE_STRICT_REALTIME
    {
        SerializedPayload_t* payload = nullptr;
        if (history_.register_instance(instance_handle, lock, max_blocking_time, payload))
        {
            // Keep serialization of sample inside the instance
            assert(nullptr != payload);
            if (0 == payload->length || nullptr == payload->data)
            {
                uint32_t size = fixed_payload_size_ ? fixed_payload_size_ : type_->getSerializedSizeProvider(key)();
                payload->reserve(size);
                if (!type_->serialize(key, payload))
                {
                    EPROSIMA_LOG_WARNING(DATA_WRITER, "Key data serialization failed");

                    // Serialization of the sample failed. Remove the instance to keep original state.
                    // Note that we will only end-up here if the instance has just been created, so it will be empty
                    // and removing its changes will remove the instance completely.
                    history_.remove_instance_changes(instance_handle, fastrtps::rtps::SequenceNumber_t());
                }
            }
            return instance_handle;
        }
    }

    return HANDLE_NIL;
}

ReturnCode_t DataWriterImpl::unregister_instance(
        void* instance,
        const InstanceHandle_t& handle,
        bool dispose)
{
    // Preconditions
    InstanceHandle_t ih;
    ReturnCode_t returned_value = check_instance_preconditions(instance, handle, ih);
    if (RETCODE_OK == returned_value && !history_.is_key_registered(ih))
    {
        returned_value = RETCODE_PRECONDITION_NOT_MET;
    }

    // Operation
    if (RETCODE_OK == returned_value)
    {
        WriteParams wparams;
        ChangeKind_t change_kind = unregister_change_kind(dispose, qos_);
        returned_value = create_new_change_with_params(change_kind, instance, wparams, ih);
    }

    return returned_value;
}

ReturnCode_t DataWriterImpl::unregister_instance_w_timestamp(
        void* instance,
        const InstanceHandle_t& handle,
        const fastrtps::Time_t& timestamp,
        bool dispose)
{
    // Preconditions
    InstanceHandle_t instance_handle;
    ReturnCode_t ret = RETCODE_OK;
    if (timestamp.is_infinite() || timestamp.seconds < 0)
    {
        ret = RETCODE_BAD_PARAMETER;
    }
    if (RETCODE_OK == ret)
    {
        ret = check_instance_preconditions(instance, handle, instance_handle);
    }
    if (RETCODE_OK == ret && !history_.is_key_registered(instance_handle))
    {
        ret = RETCODE_PRECONDITION_NOT_MET;
    }

    // Operation
    if (RETCODE_OK == ret)
    {
        WriteParams wparams;
        wparams.source_timestamp(timestamp);
        ChangeKind_t change_kind = unregister_change_kind(dispose, qos_);
        ret = create_new_change_with_params(change_kind, instance, wparams, instance_handle);
    }

    return ret;
}

ReturnCode_t DataWriterImpl::get_key_value(
        void* key_holder,
        const InstanceHandle_t& handle)
{
    /// Preconditions
    if (key_holder == nullptr || !handle.isDefined())
    {
        EPROSIMA_LOG_ERROR(DATA_WRITER, "Key holder pointer not valid");
        return RETCODE_BAD_PARAMETER;
    }

    if (!type_->m_isGetKeyDefined)
    {
        EPROSIMA_LOG_ERROR(DATA_WRITER, "Topic is NO_KEY, operation not permitted");
        return RETCODE_ILLEGAL_OPERATION;
    }

    if (writer_ == nullptr)
    {
        return RETCODE_NOT_ENABLED;
    }

    // Block lowlevel writer
#if HAVE_STRICT_REALTIME
    auto max_blocking_time = std::chrono::steady_clock::now() +
            std::chrono::microseconds(rtps::TimeConv::Time_t2MicroSecondsInt64(qos_.reliability().max_blocking_time));
    std::unique_lock<RecursiveTimedMutex> lock(writer_->getMutex(), std::defer_lock);
    if (!lock.try_lock_until(max_blocking_time))
    {
        return RETCODE_TIMEOUT;
    }
#else
    std::lock_guard<RecursiveTimedMutex> lock(writer_->getMutex());
#endif // if HAVE_STRICT_REALTIME

    SerializedPayload_t* payload = history_.get_key_value(handle);
    if (nullptr == payload)
    {
        return RETCODE_BAD_PARAMETER;
    }

    type_->deserialize(payload, key_holder);
    return RETCODE_OK;
}

ReturnCode_t DataWriterImpl::create_new_change(
        ChangeKind_t changeKind,
        void* data)
{
    WriteParams wparams;
    return create_new_change_with_params(changeKind, data, wparams);
}

ReturnCode_t DataWriterImpl::check_new_change_preconditions(
        ChangeKind_t change_kind,
        void* data)
{
    // Preconditions
    if (data == nullptr)
    {
        EPROSIMA_LOG_ERROR(DATA_WRITER, "Data pointer not valid");
        return RETCODE_BAD_PARAMETER;
    }

    if (change_kind == NOT_ALIVE_UNREGISTERED
            || change_kind == NOT_ALIVE_DISPOSED
            || change_kind == NOT_ALIVE_DISPOSED_UNREGISTERED)
    {
        if (!type_->m_isGetKeyDefined)
        {
            EPROSIMA_LOG_ERROR(DATA_WRITER, "Topic is NO_KEY, operation not permitted");
            return RETCODE_ILLEGAL_OPERATION;
        }
    }

    return RETCODE_OK;
}

ReturnCode_t DataWriterImpl::perform_create_new_change(
        ChangeKind_t change_kind,
        void* data,
        WriteParams& wparams,
        const InstanceHandle_t& handle)
{
    // Block lowlevel writer
    auto max_blocking_time = steady_clock::now() +
            microseconds(rtps::TimeConv::Time_t2MicroSecondsInt64(qos_.reliability().max_blocking_time));

#if HAVE_STRICT_REALTIME
    std::unique_lock<RecursiveTimedMutex> lock(writer_->getMutex(), std::defer_lock);
    if (!lock.try_lock_until(max_blocking_time))
    {
        return RETCODE_TIMEOUT;
    }
#else
    std::unique_lock<RecursiveTimedMutex> lock(writer_->getMutex());
#endif // if HAVE_STRICT_REALTIME

    PayloadInfo_t payload;
    bool was_loaned = check_and_remove_loan(data, payload);
    if (!was_loaned)
    {
        if (!get_free_payload_from_pool(type_->getSerializedSizeProvider(data), payload))
        {
            return RETCODE_OUT_OF_RESOURCES;
        }

        if ((ALIVE == change_kind) && !type_->serialize(data, &payload.payload, data_representation_))
        {
            EPROSIMA_LOG_WARNING(DATA_WRITER, "Data serialization returned false");
            return_payload_to_pool(payload);
            return RETCODE_ERROR;
        }
    }

    CacheChange_t* ch = writer_->new_change(change_kind, handle);
    if (ch != nullptr)
    {
        payload.move_into_change(*ch);

        bool added = false;
        if (reader_filters_)
        {
            auto related_sample_identity = wparams.related_sample_identity();
            auto filter_hook = [&related_sample_identity, this](CacheChange_t& ch)
                    {
                        reader_filters_->update_filter_info(static_cast<DataWriterFilteredChange&>(ch),
                                related_sample_identity);
                    };
            added = history_.add_pub_change_with_commit_hook(ch, wparams, filter_hook, lock, max_blocking_time);
        }
        else
        {
            added = history_.add_pub_change(ch, wparams, lock, max_blocking_time);
        }

        if (!added)
        {
            if (was_loaned)
            {
                payload.move_from_change(*ch);
                add_loan(data, payload);
            }
            writer_->release_change(ch);
            return RETCODE_TIMEOUT;
        }

        if (qos_.deadline().period != c_TimeInfinite)
        {
            if (!history_.set_next_deadline(
                        handle,
                        steady_clock::now() + duration_cast<system_clock::duration>(deadline_duration_us_)))
            {
                EPROSIMA_LOG_ERROR(DATA_WRITER, "Could not set the next deadline in the history");
            }
            else
            {
                if (timer_owner_ == handle || timer_owner_ == InstanceHandle_t())
                {
                    if (deadline_timer_reschedule())
                    {
                        deadline_timer_->cancel_timer();
                        deadline_timer_->restart_timer();
                    }
                }
            }
        }

        if (qos_.lifespan().duration != c_TimeInfinite)
        {
            lifespan_duration_us_ = duration<double, std::ratio<1, 1000000>>(
                qos_.lifespan().duration.to_ns() * 1e-3);
            lifespan_timer_->update_interval_millisec(qos_.lifespan().duration.to_ns() * 1e-6);
            lifespan_timer_->restart_timer();
        }

        return RETCODE_OK;
    }

    return RETCODE_OUT_OF_RESOURCES;
}

ReturnCode_t DataWriterImpl::create_new_change_with_params(
        ChangeKind_t changeKind,
        void* data,
        WriteParams& wparams)
{
    ReturnCode_t ret_code = check_new_change_preconditions(changeKind, data);
    if (RETCODE_OK != ret_code)
    {
        return ret_code;
    }

    InstanceHandle_t handle;
    if (type_->m_isGetKeyDefined)
    {
        bool is_key_protected = false;
#if HAVE_SECURITY
        is_key_protected = writer_->getAttributes().security_attributes().is_key_protected;
#endif // if HAVE_SECURITY
        type_->getKey(data, &handle, is_key_protected);
    }

    return perform_create_new_change(changeKind, data, wparams, handle);
}

ReturnCode_t DataWriterImpl::create_new_change_with_params(
        ChangeKind_t changeKind,
        void* data,
        WriteParams& wparams,
        const InstanceHandle_t& handle)
{
    ReturnCode_t ret_code = check_new_change_preconditions(changeKind, data);
    if (RETCODE_OK != ret_code)
    {
        return ret_code;
    }

    return perform_create_new_change(changeKind, data, wparams, handle);
}

bool DataWriterImpl::remove_min_seq_change()
{
    return history_.removeMinChange();
}

ReturnCode_t DataWriterImpl::clear_history(
        size_t* removed)
{
    return (history_.removeAllChange(removed) ? RETCODE_OK : RETCODE_ERROR);
}

ReturnCode_t DataWriterImpl::get_sending_locators(
        rtps::LocatorList& locators) const
{
    if (nullptr == writer_)
    {
        return RETCODE_NOT_ENABLED;
    }

    writer_->getRTPSParticipant()->get_sending_locators(locators);
    return RETCODE_OK;
}

const fastrtps::rtps::GUID_t& DataWriterImpl::guid() const
{
    return guid_;
}

InstanceHandle_t DataWriterImpl::get_instance_handle() const
{
    return guid();
}

void DataWriterImpl::publisher_qos_updated()
{
    if (writer_ != nullptr)
    {
        //NOTIFY THE BUILTIN PROTOCOLS THAT THE WRITER HAS CHANGED
        WriterQos wqos = qos_.get_writerqos(get_publisher()->get_qos(), topic_->get_qos());
        publisher_->rtps_participant()->updateWriter(writer_, get_topic_attributes(qos_, *topic_, type_), wqos);
    }
}

ReturnCode_t DataWriterImpl::set_qos(
        const DataWriterQos& qos)
{
    bool enabled = writer_ != nullptr;
    const DataWriterQos& qos_to_set = (&qos == &DATAWRITER_QOS_DEFAULT) ?
            publisher_->get_default_datawriter_qos() : qos;

    // Default qos is always considered consistent
    if (&qos != &DATAWRITER_QOS_DEFAULT)
    {
        ReturnCode_t ret_val = check_qos_including_resource_limits(qos_to_set, type_);
        if (RETCODE_OK != ret_val)
        {
            return ret_val;
        }

        if (publisher_->get_participant()->get_qos().allocation().data_limits.max_user_data != 0 &&
                publisher_->get_participant()->get_qos().allocation().data_limits.max_user_data <
                qos_to_set.user_data().getValue().size())
        {
            return RETCODE_INCONSISTENT_POLICY;
        }
    }

    if (enabled && !can_qos_be_updated(qos_, qos_to_set))
    {
        return RETCODE_IMMUTABLE_POLICY;
    }

    set_qos(qos_, qos_to_set, !enabled);

    if (enabled)
    {
        if (qos_.reliability().kind == ReliabilityQosPolicyKind::RELIABLE_RELIABILITY_QOS &&
                qos_.reliable_writer_qos() == qos_to_set.reliable_writer_qos())
        {
            // Update times and positive_acks attributes on RTPS Layer
            WriterAttributes w_att;
            w_att.times = qos_.reliable_writer_qos().times;
            w_att.disable_positive_acks = qos_.reliable_writer_qos().disable_positive_acks.enabled;
            w_att.keep_duration = qos_.reliable_writer_qos().disable_positive_acks.duration;
            writer_->updateAttributes(w_att);
        }

        //Notify the participant that a Writer has changed its QOS
        fastrtps::TopicAttributes topic_att = get_topic_attributes(qos_, *topic_, type_);
        WriterQos wqos = qos_.get_writerqos(get_publisher()->get_qos(), topic_->get_qos());
        publisher_->rtps_participant()->updateWriter(writer_, topic_att, wqos);

        // Deadline
        if (qos_.deadline().period != c_TimeInfinite)
        {
            deadline_duration_us_ =
                    duration<double, std::ratio<1, 1000000>>(qos_.deadline().period.to_ns() * 1e-3);
            deadline_timer_->update_interval_millisec(qos_.deadline().period.to_ns() * 1e-6);
        }
        else
        {
            deadline_timer_->cancel_timer();
        }

        // Lifespan
        if (qos_.lifespan().duration != c_TimeInfinite)
        {
            lifespan_duration_us_ =
                    duration<double, std::ratio<1, 1000000>>(qos_.lifespan().duration.to_ns() * 1e-3);
            lifespan_timer_->update_interval_millisec(qos_.lifespan().duration.to_ns() * 1e-6);
        }
        else
        {
            lifespan_timer_->cancel_timer();
        }
    }

    return RETCODE_OK;
}

const DataWriterQos& DataWriterImpl::get_qos() const
{
    return qos_;
}

ReturnCode_t DataWriterImpl::set_listener(
        DataWriterListener* listener)
{
    std::lock_guard<std::mutex> scoped_lock(listener_mutex_);
    listener_ = listener;
    return RETCODE_OK;
}

const DataWriterListener* DataWriterImpl::get_listener() const
{
    return listener_;
}

Topic* DataWriterImpl::get_topic() const
{
    return topic_;
}

const Publisher* DataWriterImpl::get_publisher() const
{
    return publisher_->get_publisher();
}

void DataWriterImpl::InnerDataWriterListener::onWriterMatched(
        RTPSWriter* /*writer*/,
        const PublicationMatchedStatus& info)
{
    data_writer_->update_publication_matched_status(info);

    StatusMask notify_status = StatusMask::publication_matched();
    DataWriterListener* listener = data_writer_->get_listener_for(notify_status);
    if (listener != nullptr)
    {
        PublicationMatchedStatus callback_status;
        if (RETCODE_OK == data_writer_->get_publication_matched_status(callback_status))
        {
            listener->on_publication_matched(data_writer_->user_datawriter_, callback_status);
        }
    }
    data_writer_->user_datawriter_->get_statuscondition().get_impl()->set_status(notify_status, true);
}

void DataWriterImpl::InnerDataWriterListener::on_offered_incompatible_qos(
        RTPSWriter* /*writer*/,
        fastdds::dds::PolicyMask qos)
{
    data_writer_->update_offered_incompatible_qos(qos);
    StatusMask notify_status = StatusMask::offered_incompatible_qos();
    DataWriterListener* listener = data_writer_->get_listener_for(notify_status);
    if (listener != nullptr)
    {
        OfferedIncompatibleQosStatus callback_status;
        if (data_writer_->get_offered_incompatible_qos_status(callback_status) == RETCODE_OK)
        {
            listener->on_offered_incompatible_qos(data_writer_->user_datawriter_, callback_status);
        }
    }

#ifdef FASTDDS_STATISTICS
    notify_status_observer(statistics::StatusKind::INCOMPATIBLE_QOS);
#endif //FASTDDS_STATISTICS

    data_writer_->user_datawriter_->get_statuscondition().get_impl()->set_status(notify_status, true);
}

void DataWriterImpl::InnerDataWriterListener::onWriterChangeReceivedByAll(
        RTPSWriter* /*writer*/,
        CacheChange_t* ch)
{
    if (data_writer_->type_->m_isGetKeyDefined &&
            (NOT_ALIVE_UNREGISTERED == ch->kind ||
            NOT_ALIVE_DISPOSED_UNREGISTERED == ch->kind))
    {
        data_writer_->history_.remove_instance_changes(ch->instanceHandle, ch->sequenceNumber);
    }
    else if (data_writer_->qos_.durability().kind == VOLATILE_DURABILITY_QOS)
    {
        data_writer_->history_.remove_change_g(ch);
    }
}

void DataWriterImpl::InnerDataWriterListener::on_liveliness_lost(
        fastrtps::rtps::RTPSWriter* /*writer*/,
        const LivelinessLostStatus& status)
{
    data_writer_->update_liveliness_lost_status(status);
    StatusMask notify_status = StatusMask::liveliness_lost();
    DataWriterListener* listener = data_writer_->get_listener_for(notify_status);
    if (listener != nullptr)
    {
        LivelinessLostStatus callback_status;
        if (RETCODE_OK == data_writer_->get_liveliness_lost_status(callback_status))
        {
            listener->on_liveliness_lost(data_writer_->user_datawriter_, callback_status);
        }
    }

#ifdef FASTDDS_STATISTICS
    notify_status_observer(statistics::StatusKind::LIVELINESS_LOST);
#endif //FASTDDS_STATISTICS

    data_writer_->user_datawriter_->get_statuscondition().get_impl()->set_status(notify_status, true);
}

void DataWriterImpl::InnerDataWriterListener::on_reader_discovery(
        fastrtps::rtps::RTPSWriter* writer,
        fastrtps::rtps::ReaderDiscoveryInfo::DISCOVERY_STATUS reason,
        const fastrtps::rtps::GUID_t& reader_guid,
        const fastrtps::rtps::ReaderProxyData* reader_info)
{
    if (!fastrtps::rtps::RTPSDomainImpl::should_intraprocess_between(writer->getGuid(), reader_guid))
    {
        switch (reason)
        {
            case fastrtps::rtps::ReaderDiscoveryInfo::DISCOVERY_STATUS::REMOVED_READER:
                data_writer_->remove_reader_filter(reader_guid);
                break;

            case fastrtps::rtps::ReaderDiscoveryInfo::DISCOVERY_STATUS::DISCOVERED_READER:
            case fastrtps::rtps::ReaderDiscoveryInfo::DISCOVERY_STATUS::CHANGED_QOS_READER:
                data_writer_->process_reader_filter_info(reader_guid, *reader_info);
                break;
            default:
                break;
        }
    }
}

#ifdef FASTDDS_STATISTICS
void DataWriterImpl::InnerDataWriterListener::notify_status_observer(
        const uint32_t& status_id)
{
    DomainParticipantImpl* pp_impl = data_writer_->publisher_->get_participant_impl();
    auto statistics_pp_impl = static_cast<eprosima::fastdds::statistics::dds::DomainParticipantImpl*>(pp_impl);
    if (nullptr != statistics_pp_impl->get_status_observer())
    {
        if (!statistics_pp_impl->get_status_observer()->on_local_entity_status_change(data_writer_->guid(), status_id))
        {
            EPROSIMA_LOG_ERROR(DATA_WRITER, "Could not set entity status");
        }
    }
}

#endif //FASTDDS_STATISTICS

ReturnCode_t DataWriterImpl::wait_for_acknowledgments(
        const Duration_t& max_wait)
{
    if (writer_ == nullptr)
    {
        return RETCODE_NOT_ENABLED;
    }

    if (writer_->wait_for_all_acked(max_wait))
    {
        return RETCODE_OK;
    }
    return RETCODE_ERROR;
}

ReturnCode_t DataWriterImpl::wait_for_acknowledgments(
        void* instance,
        const InstanceHandle_t& handle,
        const Duration_t& max_wait)
{
    // Preconditions
    InstanceHandle_t ih;
    ReturnCode_t returned_value = check_instance_preconditions(instance, handle, ih);
    if (RETCODE_OK != returned_value)
    {
        return returned_value;
    }

    // Block low-level writer
    auto max_blocking_time = steady_clock::now() +
            microseconds(rtps::TimeConv::Time_t2MicroSecondsInt64(max_wait));

# if HAVE_STRICT_REALTIME
    std::unique_lock<RecursiveTimedMutex> lock(writer_->getMutex(), std::defer_lock);
    if (!lock.try_lock_until(max_blocking_time))
    {
        return RETCODE_TIMEOUT;
    }
#else
    std::unique_lock<RecursiveTimedMutex> lock(writer_->getMutex());
#endif // HAVE_STRICT_REALTIME

    if (!history_.is_key_registered(ih))
    {
        return RETCODE_PRECONDITION_NOT_MET;
    }

    if (history_.wait_for_acknowledgement_last_change(ih, lock, max_blocking_time))
    {
        return RETCODE_OK;
    }

    return RETCODE_TIMEOUT;
}

void DataWriterImpl::update_publication_matched_status(
        const PublicationMatchedStatus& status)
{
    auto count_change = status.current_count_change;
    publication_matched_status_.current_count += count_change;
    publication_matched_status_.current_count_change += count_change;
    if (count_change > 0)
    {
        publication_matched_status_.total_count += count_change;
        publication_matched_status_.total_count_change += count_change;
    }
    publication_matched_status_.last_subscription_handle = status.last_subscription_handle;
}

ReturnCode_t DataWriterImpl::get_publication_matched_status(
        PublicationMatchedStatus& status)
{
    if (writer_ == nullptr)
    {
        return RETCODE_NOT_ENABLED;
    }

    {
        std::unique_lock<RecursiveTimedMutex> lock(writer_->getMutex());

        status = publication_matched_status_;
        publication_matched_status_.current_count_change = 0;
        publication_matched_status_.total_count_change = 0;
    }

    user_datawriter_->get_statuscondition().get_impl()->set_status(StatusMask::publication_matched(), false);
    return RETCODE_OK;
}

bool DataWriterImpl::deadline_timer_reschedule()
{
    assert(qos_.deadline().period != c_TimeInfinite);

    std::unique_lock<RecursiveTimedMutex> lock(writer_->getMutex());

    steady_clock::time_point next_deadline_us;
    if (!history_.get_next_deadline(timer_owner_, next_deadline_us))
    {
        EPROSIMA_LOG_ERROR(DATA_WRITER, "Could not get the next deadline from the history");
        return false;
    }

    auto interval_ms = duration_cast<milliseconds>(next_deadline_us - steady_clock::now());
    deadline_timer_->update_interval_millisec(static_cast<double>(interval_ms.count()));
    return true;
}

bool DataWriterImpl::deadline_missed()
{
    assert(qos_.deadline().period != c_TimeInfinite);

    std::unique_lock<RecursiveTimedMutex> lock(writer_->getMutex());

    deadline_missed_status_.total_count++;
    deadline_missed_status_.total_count_change++;
    deadline_missed_status_.last_instance_handle = timer_owner_;
    StatusMask notify_status = StatusMask::offered_deadline_missed();
    auto listener = get_listener_for(notify_status);
    if (nullptr != listener)
    {
        listener->on_offered_deadline_missed(user_datawriter_, deadline_missed_status_);
        deadline_missed_status_.total_count_change = 0;
    }

#ifdef FASTDDS_STATISTICS
    writer_listener_.notify_status_observer(statistics::StatusKind::DEADLINE_MISSED);
#endif //FASTDDS_STATISTICS

    user_datawriter_->get_statuscondition().get_impl()->set_status(notify_status, true);

    if (!history_.set_next_deadline(
                timer_owner_,
                steady_clock::now() + duration_cast<system_clock::duration>(deadline_duration_us_)))
    {
        EPROSIMA_LOG_ERROR(DATA_WRITER, "Could not set the next deadline in the history");
        return false;
    }
    return deadline_timer_reschedule();
}

ReturnCode_t DataWriterImpl::get_offered_deadline_missed_status(
        OfferedDeadlineMissedStatus& status)
{
    if (writer_ == nullptr)
    {
        return RETCODE_NOT_ENABLED;
    }

    {
        std::unique_lock<RecursiveTimedMutex> lock(writer_->getMutex());

        status = deadline_missed_status_;
        deadline_missed_status_.total_count_change = 0;
    }

    user_datawriter_->get_statuscondition().get_impl()->set_status(StatusMask::offered_deadline_missed(), false);
    return RETCODE_OK;
}

ReturnCode_t DataWriterImpl::get_offered_incompatible_qos_status(
        OfferedIncompatibleQosStatus& status)
{
    if (writer_ == nullptr)
    {
        return RETCODE_NOT_ENABLED;
    }

    {
        std::unique_lock<RecursiveTimedMutex> lock(writer_->getMutex());

        status = offered_incompatible_qos_status_;
        offered_incompatible_qos_status_.total_count_change = 0u;
    }

    user_datawriter_->get_statuscondition().get_impl()->set_status(StatusMask::offered_incompatible_qos(), false);
    return RETCODE_OK;
}

bool DataWriterImpl::lifespan_expired()
{
    std::unique_lock<RecursiveTimedMutex> lock(writer_->getMutex());

    CacheChange_t* earliest_change;
    while (history_.get_earliest_change(&earliest_change))
    {
        auto source_timestamp = system_clock::time_point() + nanoseconds(earliest_change->sourceTimestamp.to_ns());
        auto now = system_clock::now();

        // Check that the earliest change has expired (the change which started the timer could have been removed from the history)
        if (now - source_timestamp < lifespan_duration_us_)
        {
            auto interval = source_timestamp - now + lifespan_duration_us_;
            lifespan_timer_->update_interval_millisec(static_cast<double>(duration_cast<milliseconds>(interval).count()));
            return true;
        }

        // The earliest change has expired
        history_.remove_change_pub(earliest_change);

        // Set the timer for the next change if there is one
        if (!history_.get_earliest_change(&earliest_change))
        {
            return false;
        }

        // Calculate when the next change is due to expire and restart
        source_timestamp = system_clock::time_point() + nanoseconds(earliest_change->sourceTimestamp.to_ns());
        now = system_clock::now();
        auto interval = source_timestamp - now + lifespan_duration_us_;

        if (interval.count() > 0)
        {
            lifespan_timer_->update_interval_millisec(static_cast<double>(duration_cast<milliseconds>(interval).count()));
            return true;
        }
    }

    return false;
}

ReturnCode_t DataWriterImpl::get_liveliness_lost_status(
        LivelinessLostStatus& status)
{
    if (writer_ == nullptr)
    {
        return RETCODE_NOT_ENABLED;
    }

    {
        std::unique_lock<RecursiveTimedMutex> lock(writer_->getMutex());

        status = liveliness_lost_status_;
        liveliness_lost_status_.total_count_change = 0u;
    }

    user_datawriter_->get_statuscondition().get_impl()->set_status(StatusMask::liveliness_lost(), false);
    return RETCODE_OK;
}

ReturnCode_t DataWriterImpl::assert_liveliness()
{
    if (writer_ == nullptr)
    {
        return RETCODE_NOT_ENABLED;
    }

    if (!publisher_->rtps_participant()->wlp()->assert_liveliness(
                writer_->getGuid(),
                writer_->get_liveliness_kind(),
                writer_->get_liveliness_lease_duration()))
    {
        EPROSIMA_LOG_ERROR(DATAWRITER, "Could not assert liveliness of writer " << writer_->getGuid());
        return RETCODE_ERROR;
    }

    if (qos_.liveliness().kind == MANUAL_BY_TOPIC_LIVELINESS_QOS)
    {
        // As described in the RTPS specification, if liveliness kind is manual a heartbeat must be sent
        // This only applies to stateful writers, as stateless writers do not send heartbeats

        StatefulWriter* stateful_writer = dynamic_cast<StatefulWriter*>(writer_);

        if (stateful_writer != nullptr)
        {
            stateful_writer->send_periodic_heartbeat(true, true);
        }
    }
    return RETCODE_OK;
}

fastrtps::TopicAttributes DataWriterImpl::get_topic_attributes(
        const DataWriterQos& qos,
        const Topic& topic,
        const TypeSupport& type)
{
    fastrtps::TopicAttributes topic_att;
    topic_att.historyQos = qos.history();
    topic_att.resourceLimitsQos = qos.resource_limits();
    topic_att.topicName = topic.get_name();
    topic_att.topicDataType = topic.get_type_name();
    topic_att.topicKind = type->m_isGetKeyDefined ? WITH_KEY : NO_KEY;
    topic_att.auto_fill_type_information = type->auto_fill_type_information();
    if (type->type_identifier())
    {
        topic_att.type_id = *type->type_identifier();
    }
    if (type->type_object())
    {
        topic_att.type = *type->type_object();
    }
    if (type->type_information())
    {
        topic_att.type_information = *type->type_information();
    }
    return topic_att;
}

OfferedIncompatibleQosStatus& DataWriterImpl::update_offered_incompatible_qos(
        PolicyMask incompatible_policies)
{
    ++offered_incompatible_qos_status_.total_count;
    ++offered_incompatible_qos_status_.total_count_change;
    for (uint32_t id = 1; id < NEXT_QOS_POLICY_ID; ++id)
    {
        if (incompatible_policies.test(id))
        {
            ++offered_incompatible_qos_status_.policies[static_cast<QosPolicyId_t>(id)].count;
            offered_incompatible_qos_status_.last_policy_id = static_cast<QosPolicyId_t>(id);
        }
    }
    return offered_incompatible_qos_status_;
}

LivelinessLostStatus& DataWriterImpl::update_liveliness_lost_status(
        const LivelinessLostStatus& liveliness_lost_status)
{
    liveliness_lost_status_.total_count = liveliness_lost_status.total_count;
    liveliness_lost_status_.total_count_change += liveliness_lost_status.total_count_change;
    return liveliness_lost_status_;
}

void DataWriterImpl::set_qos(
        DataWriterQos& to,
        const DataWriterQos& from,
        bool update_immutable)
{
    // Check immutable policies
    if (update_immutable)
    {
        if (!(to.durability() == from.durability()))
        {
            to.durability() = from.durability();
            to.durability().hasChanged = true;
        }

        if (!(to.durability_service() == from.durability_service()))
        {
            to.durability_service() = from.durability_service();
            to.durability_service().hasChanged = true;
        }

        if (!(to.liveliness() == from.liveliness()))
        {
            to.liveliness() = from.liveliness();
            to.liveliness().hasChanged = true;
        }

        if (!(to.reliability().kind == from.reliability().kind))
        {
            to.reliability().kind = from.reliability().kind;
            to.reliability().hasChanged = true;
        }

        if (!(to.destination_order() == from.destination_order()))
        {
            to.destination_order() = from.destination_order();
            to.destination_order().hasChanged = true;
        }

        if (!(to.history() == from.history()))
        {
            to.history() = from.history();
            to.history().hasChanged = true;
        }

        if (!(to.resource_limits() == from.resource_limits()))
        {
            to.resource_limits() = from.resource_limits();
            to.resource_limits().hasChanged = true;
        }

        if (!(to.ownership() == from.ownership()))
        {
            to.ownership() = from.ownership();
            to.ownership().hasChanged = true;
        }

        to.publish_mode() = from.publish_mode();

        if (!(to.representation() == from.representation()))
        {
            to.representation() = from.representation();
            to.representation().hasChanged = true;
        }

        to.properties() = from.properties();

        if (!(to.reliable_writer_qos() == from.reliable_writer_qos()))
        {
            RTPSReliableWriterQos& rel_to = to.reliable_writer_qos();
            rel_to.disable_heartbeat_piggyback = from.reliable_writer_qos().disable_heartbeat_piggyback;
            rel_to.disable_positive_acks.enabled = from.reliable_writer_qos().disable_positive_acks.enabled;
        }

        to.endpoint() = from.endpoint();

        to.writer_resource_limits() = from.writer_resource_limits();

        to.data_sharing() = from.data_sharing();

        to.throughput_controller() = from.throughput_controller();
    }

    if (!(to.deadline() == from.deadline()))
    {
        to.deadline() = from.deadline();
        to.deadline().hasChanged = true;
    }

    if (!(to.latency_budget() == from.latency_budget()))
    {
        to.latency_budget() = from.latency_budget();
        to.latency_budget().hasChanged = true;
    }

    if (!(to.reliability().max_blocking_time == from.reliability().max_blocking_time))
    {
        to.reliability().max_blocking_time = from.reliability().max_blocking_time;
        to.reliability().hasChanged = true;
    }

    if (!(to.transport_priority() == from.transport_priority()))
    {
        to.transport_priority() = from.transport_priority();
        to.transport_priority().hasChanged = true;
    }

    if (!(to.lifespan() == from.lifespan()))
    {
        to.lifespan() = from.lifespan();
        to.lifespan().hasChanged = true;
    }

    if (!(to.user_data() == from.user_data()))
    {
        to.user_data() = from.user_data();
        to.user_data().hasChanged = true;
    }

    if (!(to.ownership_strength() == from.ownership_strength()))
    {
        to.ownership_strength() = from.ownership_strength();
        to.ownership_strength().hasChanged = true;
    }

    if (!(to.writer_data_lifecycle() == from.writer_data_lifecycle()))
    {
        to.writer_data_lifecycle() = from.writer_data_lifecycle();
    }

    if (!(to.reliable_writer_qos() == from.reliable_writer_qos()))
    {
        RTPSReliableWriterQos& rel_to = to.reliable_writer_qos();
        rel_to.times = from.reliable_writer_qos().times;
        rel_to.disable_positive_acks.duration = from.reliable_writer_qos().disable_positive_acks.duration;
    }
}

ReturnCode_t DataWriterImpl::check_qos_including_resource_limits(
        const DataWriterQos& qos,
        const TypeSupport& type)
{
    ReturnCode_t check_qos_return = check_qos(qos);
    if (RETCODE_OK == check_qos_return &&
            type->m_isGetKeyDefined)
    {
        check_qos_return = check_allocation_consistency(qos);
    }
    return check_qos_return;
}

ReturnCode_t DataWriterImpl::check_qos(
        const DataWriterQos& qos)
{
    if (qos.durability().kind == PERSISTENT_DURABILITY_QOS)
    {
        EPROSIMA_LOG_ERROR(RTPS_QOS_CHECK, "PERSISTENT Durability not supported");
        return RETCODE_UNSUPPORTED;
    }
    if (qos.destination_order().kind == BY_SOURCE_TIMESTAMP_DESTINATIONORDER_QOS)
    {
        EPROSIMA_LOG_ERROR(RTPS_QOS_CHECK, "BY SOURCE TIMESTAMP DestinationOrder not supported");
        return RETCODE_UNSUPPORTED;
    }
    if (nullptr != PropertyPolicyHelper::find_property(qos.properties(), "fastdds.unique_network_flows"))
    {
        EPROSIMA_LOG_ERROR(RTPS_QOS_CHECK, "Unique network flows not supported on writers");
        return RETCODE_UNSUPPORTED;
    }
    bool is_pull_mode = qos_has_pull_mode_request(qos);
    if (is_pull_mode)
    {
        if (BEST_EFFORT_RELIABILITY_QOS == qos.reliability().kind)
        {
            EPROSIMA_LOG_ERROR(RTPS_QOS_CHECK, "BEST_EFFORT incompatible with pull mode");
            return RETCODE_INCONSISTENT_POLICY;
        }
        if (c_TimeInfinite == qos.reliable_writer_qos().times.heartbeatPeriod)
        {
            EPROSIMA_LOG_ERROR(RTPS_QOS_CHECK, "Infinite heartbeat period incompatible with pull mode");
            return RETCODE_INCONSISTENT_POLICY;
        }
    }
    if (qos.liveliness().kind == AUTOMATIC_LIVELINESS_QOS ||
            qos.liveliness().kind == MANUAL_BY_PARTICIPANT_LIVELINESS_QOS)
    {
        if (qos.liveliness().lease_duration < eprosima::fastrtps::c_TimeInfinite &&
                qos.liveliness().lease_duration <= qos.liveliness().announcement_period)
        {
            EPROSIMA_LOG_ERROR(RTPS_QOS_CHECK, "WRITERQOS: LeaseDuration <= announcement period.");
            return RETCODE_INCONSISTENT_POLICY;
        }
    }
    if (qos.data_sharing().kind() == DataSharingKind::ON &&
            (qos.endpoint().history_memory_policy != PREALLOCATED_MEMORY_MODE &&
            qos.endpoint().history_memory_policy != PREALLOCATED_WITH_REALLOC_MEMORY_MODE))
    {
        EPROSIMA_LOG_ERROR(RTPS_QOS_CHECK, "DATA_SHARING cannot be used with memory policies other than PREALLOCATED.");
        return RETCODE_INCONSISTENT_POLICY;
    }
    if (qos.history().kind == KEEP_LAST_HISTORY_QOS && qos.history().depth <= 0)
    {
        EPROSIMA_LOG_ERROR(RTPS_QOS_CHECK, "HISTORY DEPTH must be higher than 0 if HISTORY KIND is KEEP_LAST.");
        return RETCODE_INCONSISTENT_POLICY;
    }
    if (qos.history().kind == KEEP_LAST_HISTORY_QOS && qos.history().depth > 0 &&
            qos.resource_limits().max_samples_per_instance > 0 &&
            qos.history().depth > qos.resource_limits().max_samples_per_instance)
    {
        EPROSIMA_LOG_WARNING(RTPS_QOS_CHECK,
                "HISTORY DEPTH '" << qos.history().depth <<
                "' is inconsistent with max_samples_per_instance: '" << qos.resource_limits().max_samples_per_instance <<
                "'. Consistency rule: depth <= max_samples_per_instance." <<
                " Effectively using max_samples_per_instance as depth.");
    }
    return RETCODE_OK;
}

ReturnCode_t DataWriterImpl::check_allocation_consistency(
        const DataWriterQos& qos)
{
    if ((qos.resource_limits().max_samples > 0) &&
            (qos.resource_limits().max_samples <
            (qos.resource_limits().max_instances * qos.resource_limits().max_samples_per_instance)))
    {
        EPROSIMA_LOG_ERROR(DDS_QOS_CHECK,
                "max_samples should be greater than max_instances * max_samples_per_instance");
        return RETCODE_INCONSISTENT_POLICY;
    }
    if ((qos.resource_limits().max_instances <= 0 || qos.resource_limits().max_samples_per_instance <= 0) &&
            (qos.resource_limits().max_samples > 0))
    {
        EPROSIMA_LOG_ERROR(DDS_QOS_CHECK,
                "max_samples should be infinite when max_instances or max_samples_per_instance are infinite");
        return RETCODE_INCONSISTENT_POLICY;
    }
    return RETCODE_OK;
}

bool DataWriterImpl::can_qos_be_updated(
        const DataWriterQos& to,
        const DataWriterQos& from)
{
    bool updatable = true;
    if (to.durability().kind != from.durability().kind)
    {
        updatable = false;
        EPROSIMA_LOG_WARNING(RTPS_QOS_CHECK, "Durability kind cannot be changed after the creation of a DataWriter.");
    }

    if (to.liveliness().kind !=  from.liveliness().kind)
    {
        updatable = false;
        EPROSIMA_LOG_WARNING(RTPS_QOS_CHECK, "Liveliness Kind cannot be changed after the creation of a DataWriter.");
    }

    if (to.liveliness().lease_duration != from.liveliness().lease_duration)
    {
        updatable = false;
        EPROSIMA_LOG_WARNING(RTPS_QOS_CHECK,
                "Liveliness lease duration cannot be changed after the creation of a DataWriter.");
    }

    if (to.liveliness().announcement_period != from.liveliness().announcement_period)
    {
        updatable = false;
        EPROSIMA_LOG_WARNING(RTPS_QOS_CHECK,
                "Liveliness announcement cannot be changed after the creation of a DataWriter.");
    }

    if (to.reliability().kind != from.reliability().kind)
    {
        updatable = false;
        EPROSIMA_LOG_WARNING(RTPS_QOS_CHECK, "Reliability Kind cannot be changed after the creation of a DataWriter.");
    }
    if (to.ownership().kind != from.ownership().kind)
    {
        updatable = false;
        EPROSIMA_LOG_WARNING(RTPS_QOS_CHECK, "Ownership Kind cannot be changed after the creation of a DataWriter.");
    }
    if (to.destination_order().kind != from.destination_order().kind)
    {
        updatable = false;
        EPROSIMA_LOG_WARNING(RTPS_QOS_CHECK,
                "Destination order Kind cannot be changed after the creation of a DataWriter.");
    }
    if (to.data_sharing().kind() != from.data_sharing().kind())
    {
        updatable = false;
        EPROSIMA_LOG_WARNING(RTPS_QOS_CHECK,
                "Data sharing configuration cannot be changed after the creation of a DataWriter.");
    }
    if (to.data_sharing().shm_directory() != from.data_sharing().shm_directory())
    {
        updatable = false;
        EPROSIMA_LOG_WARNING(RTPS_QOS_CHECK,
                "Data sharing configuration cannot be changed after the creation of a DataWriter.");
    }
    if (to.data_sharing().domain_ids() != from.data_sharing().domain_ids())
    {
        updatable = false;
        EPROSIMA_LOG_WARNING(RTPS_QOS_CHECK,
                "Data sharing configuration cannot be changed after the creation of a DataWriter.");
    }
    if (to.reliable_writer_qos().disable_positive_acks.enabled !=
            from.reliable_writer_qos().disable_positive_acks.enabled)
    {
        updatable = false;
        EPROSIMA_LOG_WARNING(RTPS_QOS_CHECK,
                "Only the period of Positive ACKs can be changed after the creation of a DataWriter.");
    }
    return updatable;
}

DataWriterListener* DataWriterImpl::get_listener_for(
        const StatusMask& status)
{
    std::lock_guard<std::mutex> scoped_lock(listener_mutex_);
    if (listener_ != nullptr &&
            user_datawriter_->get_status_mask().is_active(status))
    {
        return listener_;
    }
    return publisher_->get_listener_for(status);
}

std::shared_ptr<IChangePool> DataWriterImpl::get_change_pool() const
{
    PoolConfig config = PoolConfig::from_history_attributes(history_.m_att);
    if (reader_filters_)
    {
        return std::make_shared<DataWriterFilteredChangePool>(
            config, qos_.writer_resource_limits().reader_filters_allocation);
    }

    return std::make_shared<fastrtps::rtps::CacheChangePool>(config);
}

std::shared_ptr<IPayloadPool> DataWriterImpl::get_payload_pool()
{
    if (!payload_pool_)
    {
        // When the user requested PREALLOCATED_WITH_REALLOC, but we know the type cannot
        // grow, we translate the policy into bare PREALLOCATED
        if (PREALLOCATED_WITH_REALLOC_MEMORY_MODE == history_.m_att.memoryPolicy &&
                (type_->is_bounded() || type_->is_plain(data_representation_)))
        {
            history_.m_att.memoryPolicy = PREALLOCATED_MEMORY_MODE;
        }

        PoolConfig config = PoolConfig::from_history_attributes(history_.m_att);

        // Avoid calling the serialization size functors on PREALLOCATED mode
        fixed_payload_size_ = config.memory_policy == PREALLOCATED_MEMORY_MODE ? config.payload_initial_size : 0u;

        // Get payload pool reference and allocate space for our history
        if (is_data_sharing_compatible_)
        {
            payload_pool_ = DataSharingPayloadPool::get_writer_pool(config);
        }
        else
        {
            payload_pool_ = TopicPayloadPoolRegistry::get(topic_->get_name(), config);
            if (!std::static_pointer_cast<ITopicPayloadPool>(payload_pool_)->reserve_history(config, false))
            {
                payload_pool_.reset();
            }
        }

        // Prepare loans collection for plain types only
        if (type_->is_plain(data_representation_))
        {
            loans_.reset(new LoanCollection(config));
        }
    }

    return payload_pool_;
}

bool DataWriterImpl::release_payload_pool()
{
    assert(payload_pool_);

    loans_.reset();

    bool result = true;

    if (is_data_sharing_compatible_ || is_custom_payload_pool_)
    {
        // No-op
    }
    else
    {
        PoolConfig config = PoolConfig::from_history_attributes(history_.m_att);
        auto topic_pool = std::static_pointer_cast<ITopicPayloadPool>(payload_pool_);
        result = topic_pool->release_history(config, false);
    }

    payload_pool_.reset();

    return result;
}

bool DataWriterImpl::add_loan(
        void* data,
        PayloadInfo_t& payload)
{
    return loans_ && loans_->add_loan(data, payload);
}

bool DataWriterImpl::check_and_remove_loan(
        void* data,
        PayloadInfo_t& payload)
{
    return loans_ && loans_->check_and_remove_loan(data, payload);
}

ReturnCode_t DataWriterImpl::check_datasharing_compatible(
        const WriterAttributes& writer_attributes,
        bool& is_datasharing_compatible) const
{

#if HAVE_SECURITY
    bool has_security_enabled = publisher_->rtps_participant()->is_security_enabled_for_writer(writer_attributes);
#else
    (void) writer_attributes;
#endif // HAVE_SECURITY

    bool has_bound_payload_size =
            (qos_.endpoint().history_memory_policy == eprosima::fastrtps::rtps::PREALLOCATED_MEMORY_MODE ||
            qos_.endpoint().history_memory_policy == eprosima::fastrtps::rtps::PREALLOCATED_WITH_REALLOC_MEMORY_MODE) &&
            type_.is_bounded();

    bool has_key = type_->m_isGetKeyDefined;

    is_datasharing_compatible = false;
    switch (qos_.data_sharing().kind())
    {
        case DataSharingKind::OFF:
            return RETCODE_OK;
            break;
        case DataSharingKind::ON:
            if (is_custom_payload_pool_)
            {
                EPROSIMA_LOG_ERROR(DATA_WRITER, "Custom payload pool detected. Cannot force Data sharing usage.");
                return RETCODE_INCONSISTENT_POLICY;
            }
#if HAVE_SECURITY
            if (has_security_enabled)
            {
                EPROSIMA_LOG_ERROR(DATA_WRITER, "Data sharing cannot be used with security protection.");
                return RETCODE_NOT_ALLOWED_BY_SECURITY;
            }
#endif // HAVE_SECURITY

            if (!has_bound_payload_size)
            {
                EPROSIMA_LOG_ERROR(DATA_WRITER, "Data sharing cannot be used with " <<
                        (type_.is_bounded() ? "memory policies other than PREALLOCATED" : "unbounded data types"));
                return RETCODE_BAD_PARAMETER;
            }

            if (has_key)
            {
                EPROSIMA_LOG_ERROR(DATA_WRITER, "Data sharing cannot be used with keyed data types");
                return RETCODE_BAD_PARAMETER;
            }

            is_datasharing_compatible = true;
            return RETCODE_OK;
            break;
        case DataSharingKind::AUTO:
            if (is_custom_payload_pool_)
            {
                EPROSIMA_LOG_INFO(DATA_WRITER, "Custom payload pool detected. Data Sharing disabled.");
                return RETCODE_OK;
            }
#if HAVE_SECURITY
            if (has_security_enabled)
            {
                EPROSIMA_LOG_INFO(DATA_WRITER, "Data sharing disabled due to security configuration.");
                return RETCODE_OK;
            }
#endif // HAVE_SECURITY

            if (!has_bound_payload_size)
            {
                EPROSIMA_LOG_INFO(DATA_WRITER, "Data sharing disabled because " <<
                        (type_.is_bounded() ? "memory policy is not PREALLOCATED" : "data type is not bounded"));
                return RETCODE_OK;
            }

            if (has_key)
            {
                EPROSIMA_LOG_INFO(DATA_WRITER, "Data sharing disabled because data type is keyed");
                return RETCODE_OK;
            }

            is_datasharing_compatible = true;
            return RETCODE_OK;
            break;
        default:
            EPROSIMA_LOG_ERROR(DATA_WRITER, "Unknown data sharing kind.");
            return RETCODE_BAD_PARAMETER;
    }
}

void DataWriterImpl::remove_reader_filter(
        const fastrtps::rtps::GUID_t& reader_guid)
{
    if (reader_filters_)
    {
        reader_filters_->remove_reader(reader_guid);
    }
}

void DataWriterImpl::process_reader_filter_info(
        const fastrtps::rtps::GUID_t& reader_guid,
        const fastrtps::rtps::ReaderProxyData& reader_info)
{
    if (reader_filters_ &&
            !writer_->is_datasharing_compatible_with(reader_info) &&
            reader_info.remote_locators().multicast.empty())
    {
        reader_filters_->process_reader_filter_info(reader_guid, reader_info.content_filter(),
                publisher_->get_participant_impl(), topic_);
    }
}

void DataWriterImpl::filter_is_being_removed(
        const char* filter_class_name)
{
    if (reader_filters_)
    {
        assert(writer_);
        std::lock_guard<RecursiveTimedMutex> guard(writer_->getMutex());
        reader_filters_->remove_filters(filter_class_name);
    }
}

bool DataWriterImpl::is_relevant(
        const fastrtps::rtps::CacheChange_t& change,
        const fastrtps::rtps::GUID_t& reader_guid) const
{
    assert(reader_filters_);
    const DataWriterFilteredChange& writer_change = static_cast<const DataWriterFilteredChange&>(change);
    return writer_change.is_relevant_for(reader_guid);
}

} // namespace dds
} // namespace fastdds
} // namespace eprosima
