// Copyright 2016 Proyectos y Sistemas de Mantenimiento SL (eProsima).
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

/**
 * @file SubscriberImpl.cpp
 *
 */

#include "SubscriberImpl.h"
#include <fastrtps/subscriber/Subscriber.h>
#include <fastrtps/TopicDataType.h>
#include <fastrtps/subscriber/SubscriberListener.h>
#include <fastrtps/rtps/reader/RTPSReader.h>
#include <fastrtps/rtps/reader/StatefulReader.h>
#include <fastrtps/rtps/RTPSDomain.h>
#include <fastrtps/rtps/participant/RTPSParticipant.h>
#include <fastrtps/rtps/resources/ResourceEvent.h>
#include "../participant/ParticipantImpl.h"

#include <fastrtps/log/Log.h>

using namespace eprosima::fastrtps::rtps;
using namespace std::chrono;

namespace eprosima {
namespace fastrtps {


SubscriberImpl::SubscriberImpl(
        ParticipantImpl* p,
        TopicDataType* ptype,
        const SubscriberAttributes& att,
        SubscriberListener* listen)
    : mp_participant(p)
    , mp_reader(nullptr)
    , mp_type(ptype)
    , m_att(att)
#pragma warning (disable : 4355 )
    , m_history(this,ptype->m_typeSize  + 3/*Possible alignment*/, att.topic.historyQos, att.topic.resourceLimitsQos,att.historyMemoryPolicy)
    , mp_listener(listen)
    , m_readerListener(this)
    , mp_userSubscriber(nullptr)
    , mp_rtpsParticipant(nullptr)
    , lifespan_timer_(std::bind(&SubscriberImpl::lifespan_expired, this),
                      m_att.qos.m_lifespan.duration.to_ns() * 1e-6,
                      mp_participant->get_resource_event().getIOService(),
                      mp_participant->get_resource_event().getThread())
    , lifespan_duration_us_(m_att.qos.m_lifespan.duration.to_ns() * 1e-3)
    {

    }


SubscriberImpl::~SubscriberImpl()
{
    if(mp_reader != nullptr)
    {
        logInfo(SUBSCRIBER,this->getGuid().entityId << " in topic: "<<this->m_att.topic.topicName);
    }

    RTPSDomain::removeRTPSReader(mp_reader);
    delete(this->mp_userSubscriber);
}


void SubscriberImpl::waitForUnreadMessage()
{
    if(m_history.getUnreadCount()==0)
    {
        do
        {
            m_history.waitSemaphore();
        }
        while(m_history.getUnreadCount() == 0);
    }
}



bool SubscriberImpl::readNextData(void* data,SampleInfo_t* info)
{
    return this->m_history.readNextData(data,info);
}

bool SubscriberImpl::takeNextData(void* data,SampleInfo_t* info)
{
    return this->m_history.takeNextData(data,info);
}



const GUID_t& SubscriberImpl::getGuid(){
    return mp_reader->getGuid();
}



bool SubscriberImpl::updateAttributes(const SubscriberAttributes& att)
{
    bool updated = true;
    bool missing = false;
    if(att.unicastLocatorList.size() != this->m_att.unicastLocatorList.size() ||
            att.multicastLocatorList.size() != this->m_att.multicastLocatorList.size())
    {
        logWarning(RTPS_READER,"Locator Lists cannot be changed or updated in this version");
        updated &= false;
    }
    else
    {
        for(LocatorListConstIterator lit1 = this->m_att.unicastLocatorList.begin();
                lit1!=this->m_att.unicastLocatorList.end();++lit1)
        {
            missing = true;
            for(LocatorListConstIterator lit2 = att.unicastLocatorList.begin();
                    lit2!= att.unicastLocatorList.end();++lit2)
            {
                if(*lit1 == *lit2)
                {
                    missing = false;
                    break;
                }
            }
            if(missing)
            {
                logWarning(RTPS_READER,"Locator: "<< *lit1 << " not present in new list");
                logWarning(RTPS_READER,"Locator Lists cannot be changed or updated in this version");
            }
        }
        for(LocatorListConstIterator lit1 = this->m_att.multicastLocatorList.begin();
                lit1!=this->m_att.multicastLocatorList.end();++lit1)
        {
            missing = true;
            for(LocatorListConstIterator lit2 = att.multicastLocatorList.begin();
                    lit2!= att.multicastLocatorList.end();++lit2)
            {
                if(*lit1 == *lit2)
                {
                    missing = false;
                    break;
                }
            }
            if(missing)
            {
                logWarning(RTPS_READER,"Locator: "<< *lit1<< " not present in new list");
                logWarning(RTPS_READER,"Locator Lists cannot be changed or updated in this version");
            }
        }
    }

    //TOPIC ATTRIBUTES
    if(this->m_att.topic != att.topic)
    {
        logWarning(RTPS_READER,"Topic Attributes cannot be updated");
        updated &= false;
    }
    //QOS:
    //CHECK IF THE QOS CAN BE SET
    if(!this->m_att.qos.canQosBeUpdated(att.qos))
    {
        updated &=false;
    }
    if(updated)
    {
        this->m_att.expectsInlineQos = att.expectsInlineQos;
        if(this->m_att.qos.m_reliability.kind == RELIABLE_RELIABILITY_QOS)
        {
            //UPDATE TIMES:
            StatefulReader* sfr = (StatefulReader*)mp_reader;
            sfr->updateTimes(att.times);
        }
        this->m_att.qos.setQos(att.qos,false);
        //NOTIFY THE BUILTIN PROTOCOLS THAT THE READER HAS CHANGED
        mp_rtpsParticipant->updateReader(this->mp_reader, m_att.topic, m_att.qos);
    }

    // Update lifespan
    if (m_att.qos.m_lifespan.duration == c_TimeInfinite)
    {
        lifespan_duration_us_ = std::chrono::duration<double, std::ratio<1, 1000000>>(m_att.qos.m_lifespan.duration.to_ns() * 1e-3);
        lifespan_timer_.update_interval_millisec(m_att.qos.m_lifespan.duration.to_ns() * 1e-6);
        lifespan_timer_.restart_timer();
    }

    return updated;
}

void SubscriberImpl::SubscriberReaderListener::onNewCacheChangeAdded(RTPSReader* /*reader*/, CacheChange_t * const change)
{
    if (mp_subscriberImpl->onNewCacheChangeAdded(change))
    {
        if(mp_subscriberImpl->mp_listener != nullptr)
        {
            //cout << "FIRST BYTE: "<< (int)change->serializedPayload.data[0] << endl;
            mp_subscriberImpl->mp_listener->onNewDataMessage(mp_subscriberImpl->mp_userSubscriber);
        }
    }
}

void SubscriberImpl::SubscriberReaderListener::onReaderMatched(RTPSReader* /*reader*/, MatchingInfo& info)
{
    if (this->mp_subscriberImpl->mp_listener != nullptr)
    {
        mp_subscriberImpl->mp_listener->onSubscriptionMatched(mp_subscriberImpl->mp_userSubscriber,info);
    }
}

bool SubscriberImpl::onNewCacheChangeAdded(CacheChange_t * const change)
{
    // Check if the added change has expired due to lifespan
    // If so, remove it from the history
    // Otherwise start the lifespan timer

    steady_clock::time_point source_timestamp = steady_clock::time_point() + nanoseconds(change->sourceTimestamp.to_ns());
    steady_clock::time_point now = steady_clock::now();

    if (now - source_timestamp >= lifespan_duration_us_)
    {
        // Change expired
        m_history.remove_change_sub(change);
        return false;
    }

    steady_clock::duration interval = source_timestamp - now + duration_cast<nanoseconds>(lifespan_duration_us_);

    lifespan_timer_.update_interval_millisec(interval.count() * 1e-6);
    lifespan_timer_.restart_timer();
    return true;
}

/*!
 * @brief Returns there is a clean state with all Publishers.
 * It occurs when the Subscriber received all samples sent by Publishers. In other words,
 * its WriterProxies are up to date.
 * @return There is a clean state with all Publishers.
 */
bool SubscriberImpl::isInCleanState() const
{
    return mp_reader->isInCleanState();
}

uint64_t SubscriberImpl::getUnreadCount() const
{
    return m_history.getUnreadCount();
}

void SubscriberImpl::lifespan_expired()
{
    CacheChange_t* earliest_change;
    if (!m_history.get_earliest_change(&earliest_change))
    {
        return;
    }
    m_history.remove_change_sub(earliest_change);
    if (!m_history.get_earliest_change(&earliest_change))
    {
        return;
    }

    steady_clock::time_point source_timestamp = steady_clock::time_point() + nanoseconds(earliest_change->sourceTimestamp.to_ns());
    steady_clock::time_point now = steady_clock::now();

    // Calculate when the next earliest change is due to expire and restart the timer
    steady_clock::duration interval = source_timestamp - now + duration_cast<nanoseconds>(lifespan_duration_us_);
    lifespan_timer_.update_interval_millisec(interval.count() * 1e-6);
    lifespan_timer_.restart_timer();
}

} /* namespace fastrtps */
} /* namespace eprosima */
