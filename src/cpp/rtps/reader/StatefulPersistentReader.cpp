// Copyright 2018 Proyectos y Sistemas de Mantenimiento SL (eProsima).
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
 * @file StatefulPersistentReader.cpp
 *
 */

#include <fastdds/rtps/reader/StatefulPersistentReader.h>

#include <fastdds/rtps/history/ReaderHistory.h>

#include <rtps/persistence/PersistenceService.h>
#include <rtps/reader/ReaderHistoryState.hpp>

namespace eprosima {
namespace fastrtps {
namespace rtps {

StatefulPersistentReader::StatefulPersistentReader(
        RTPSParticipantImpl* impl,
        const GUID_t& guid,
        const ReaderAttributes& att,
        ReaderHistory* hist,
        ReaderListener* listen,
        IPersistenceService* persistence)
    : StatefulReader(impl, guid, att, hist, listen)
    , persistence_(persistence)
    , persistence_guid_()
{
    init(guid, att);
}

StatefulPersistentReader::StatefulPersistentReader(
        RTPSParticipantImpl* impl,
        const GUID_t& guid,
        const ReaderAttributes& att,
        const std::shared_ptr<IPayloadPool>& payload_pool,
        ReaderHistory* hist,
        ReaderListener* listen,
        IPersistenceService* persistence)
    : StatefulReader(impl, guid, att, payload_pool, hist, listen)
    , persistence_(persistence)
    , persistence_guid_()
{
    init(guid, att);
}

StatefulPersistentReader::StatefulPersistentReader(
        RTPSParticipantImpl* impl,
        const GUID_t& guid,
        const ReaderAttributes& att,
        const std::shared_ptr<IPayloadPool>& payload_pool,
        const std::shared_ptr<IChangePool>& change_pool,
        ReaderHistory* hist,
        ReaderListener* listen,
        IPersistenceService* persistence)
    : StatefulReader(impl, guid, att, payload_pool, change_pool, hist, listen)
    , persistence_(persistence)
    , persistence_guid_()
{
    init(guid, att);
}

void StatefulPersistentReader::init(
        const GUID_t& guid,
        const ReaderAttributes& att)
{
    // When persistence GUID is unknown, create from rtps GUID
    GUID_t p_guid = att.endpoint.persistence_guid == c_Guid_Unknown ? guid : att.endpoint.persistence_guid;
    std::ostringstream ss;
    ss << p_guid;
    persistence_guid_ = ss.str();
    persistence_->load_reader_from_storage(persistence_guid_, history_state_->history_record);
}

StatefulPersistentReader::~StatefulPersistentReader()
{
    delete persistence_;
}

void StatefulPersistentReader::set_last_notified(
        const GUID_t& writer_guid,
        const SequenceNumber_t& seq)
{
    history_state_->history_record[writer_guid] = seq;
    persistence_->update_writer_seq_on_storage(persistence_guid_, writer_guid, seq);
}

bool StatefulPersistentReader::may_remove_history_record(
        bool /* removed_by_lease */)
{
    return false;
}

} /* namespace rtps */
} /* namespace fastrtps */
} /* namespace eprosima */
