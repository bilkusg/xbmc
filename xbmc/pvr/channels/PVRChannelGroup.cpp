/*
 *  Copyright (C) 2012-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

//! @todo use Observable here, so we can use event driven operations later

#include "PVRChannelGroup.h"

#include "ServiceBroker.h"
#include "addons/kodi-dev-kit/include/kodi/c-api/addon-instance/pvr/pvr_channel_groups.h"
#include "pvr/PVRDatabase.h"
#include "pvr/PVRManager.h"
#include "pvr/addons/PVRClient.h"
#include "pvr/addons/PVRClients.h"
#include "pvr/channels/PVRChannel.h"
#include "pvr/channels/PVRChannelGroupMember.h"
#include "pvr/channels/PVRChannelsPath.h"
#include "pvr/epg/Epg.h"
#include "pvr/epg/EpgChannelData.h"
#include "pvr/epg/EpgInfoTag.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/Setting.h"
#include "threads/SingleLock.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace PVR;

CPVRChannelGroup::CPVRChannelGroup(const CPVRChannelsPath& path,
                                   int iGroupId /* = INVALID_GROUP_ID */,
                                   const std::shared_ptr<CPVRChannelGroup>& allChannelsGroup /* = {} */)
  : m_iGroupId(iGroupId)
  , m_allChannelsGroup(allChannelsGroup)
  , m_path(path)
{
  OnInit();
}

CPVRChannelGroup::CPVRChannelGroup(const PVR_CHANNEL_GROUP& group,
                                   const std::shared_ptr<CPVRChannelGroup>& allChannelsGroup)
  : m_iPosition(group.iPosition)
  , m_allChannelsGroup(allChannelsGroup)
  , m_path(group.bIsRadio, group.strGroupName)
{
  OnInit();
}

CPVRChannelGroup::~CPVRChannelGroup()
{
  CServiceBroker::GetSettingsComponent()->GetSettings()->UnregisterCallback(this);
  Unload();
}

bool CPVRChannelGroup::operator==(const CPVRChannelGroup& right) const
{
  return (m_iGroupType == right.m_iGroupType &&
          m_iGroupId == right.m_iGroupId &&
          m_iPosition == right.m_iPosition &&
          m_path == right.m_path);
}

bool CPVRChannelGroup::operator!=(const CPVRChannelGroup& right) const
{
  return !(*this == right);
}

std::shared_ptr<CPVRChannelGroupMember> CPVRChannelGroup::EmptyMember =
    std::make_shared<CPVRChannelGroupMember>();

void CPVRChannelGroup::OnInit()
{
  CServiceBroker::GetSettingsComponent()->GetSettings()->RegisterCallback(this, {
    CSettings::SETTING_PVRMANAGER_BACKENDCHANNELORDER,
    CSettings::SETTING_PVRMANAGER_USEBACKENDCHANNELNUMBERS,
    CSettings::SETTING_PVRMANAGER_USEBACKENDCHANNELNUMBERSALWAYS,
    CSettings::SETTING_PVRMANAGER_STARTGROUPCHANNELNUMBERSFROMONE
  });
}

namespace
{

bool UsingBackendChannelNumbers(const std::shared_ptr<CSettings>& settings)
{
  int enabledClientAmount = CServiceBroker::GetPVRManager().Clients()->EnabledClientAmount();
  return settings->GetBool(CSettings::SETTING_PVRMANAGER_USEBACKENDCHANNELNUMBERS) &&
         (enabledClientAmount == 1 ||
          (settings->GetBool(CSettings::SETTING_PVRMANAGER_USEBACKENDCHANNELNUMBERSALWAYS) &&
           enabledClientAmount > 1));
}

} // unnamed namespace

bool CPVRChannelGroup::Load(std::vector<std::shared_ptr<CPVRChannel>>& channelsToRemove)
{
  /* make sure this container is empty before loading */
  Unload();

  const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  m_bSyncChannelGroups = settings->GetBool(CSettings::SETTING_PVRMANAGER_SYNCCHANNELGROUPS);
  m_bUsingBackendChannelOrder = settings->GetBool(CSettings::SETTING_PVRMANAGER_BACKENDCHANNELORDER);
  m_bUsingBackendChannelNumbers = UsingBackendChannelNumbers(settings);
  m_bStartGroupChannelNumbersFromOne = settings->GetBool(CSettings::SETTING_PVRMANAGER_STARTGROUPCHANNELNUMBERSFROMONE) && !m_bUsingBackendChannelNumbers;

  int iChannelCount = m_iGroupId > 0 ? LoadFromDb() : 0;
  CLog::LogFC(LOGDEBUG, LOGPVR, "{} channels loaded from the database for group '{}'",
              iChannelCount, GroupName());

  if (!Update(channelsToRemove))
  {
    CLog::LogF(LOGERROR, "Failed to update channels for group '{}'", GroupName());
    return false;
  }

  if (Size() - iChannelCount > 0)
  {
    CLog::LogFC(LOGDEBUG, LOGPVR, "{} channels added from clients to group '{}'",
                static_cast<int>(Size() - iChannelCount), GroupName());
  }

  SortAndRenumber();

  m_bLoaded = true;

  return true;
}

void CPVRChannelGroup::Unload()
{
  CSingleLock lock(m_critSection);
  m_sortedMembers.clear();
  m_members.clear();
  m_failedClients.clear();
}

bool CPVRChannelGroup::Update(std::vector<std::shared_ptr<CPVRChannel>>& channelsToRemove)
{
  if (GroupType() == PVR_GROUP_TYPE_USER_DEFINED ||
      !CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_PVRMANAGER_SYNCCHANNELGROUPS))
    return true;

  CPVRChannelGroup PVRChannels_tmp(m_path, m_iGroupId, m_allChannelsGroup);
  PVRChannels_tmp.SetPreventSortAndRenumber();
  PVRChannels_tmp.LoadFromClients();
  m_failedClients = PVRChannels_tmp.m_failedClients;
  return UpdateGroupEntries(PVRChannels_tmp, channelsToRemove);
}

const CPVRChannelsPath& CPVRChannelGroup::GetPath() const
{
  CSingleLock lock(m_critSection);
  return m_path;
}

void CPVRChannelGroup::SetPath(const CPVRChannelsPath& path)
{
  CSingleLock lock(m_critSection);
  if (m_path != path)
  {
    m_path = path;
    if (m_bLoaded)
    {
      // note: path contains both the radio flag and the group name, which are stored in the db
      m_bChanged = true;
      Persist(); //! @todo why must we persist immediately?
    }
  }
}

bool CPVRChannelGroup::SetChannelNumber(const std::shared_ptr<CPVRChannel>& channel, const CPVRChannelNumber& channelNumber)
{
  bool bReturn(false);
  CSingleLock lock(m_critSection);

  for (auto& member : m_sortedMembers)
  {
    if (*member->Channel() == *channel)
    {
      if (member->ChannelNumber() != channelNumber)
      {
        bReturn = true;
        member->SetChannelNumber(channelNumber);
      }
      break;
    }
  }

  return bReturn;
}

/********** sort methods **********/

struct sortByClientChannelNumber
{
  bool operator()(const std::shared_ptr<CPVRChannelGroupMember>& channel1,
                  const std::shared_ptr<CPVRChannelGroupMember>& channel2) const
  {
    if (channel1->ClientPriority() == channel2->ClientPriority())
    {
      if (channel1->ClientChannelNumber() == channel2->ClientChannelNumber())
        return channel1->Channel()->ChannelName() < channel2->Channel()->ChannelName();

      return channel1->ClientChannelNumber() < channel2->ClientChannelNumber();
    }
    return channel1->ClientPriority() > channel2->ClientPriority();
  }
};

struct sortByChannelNumber
{
  bool operator()(const std::shared_ptr<CPVRChannelGroupMember>& channel1,
                  const std::shared_ptr<CPVRChannelGroupMember>& channel2) const
  {
    return channel1->ChannelNumber() < channel2->ChannelNumber();
  }
};

void CPVRChannelGroup::Sort()
{
  if (m_bUsingBackendChannelOrder)
    SortByClientChannelNumber();
  else
    SortByChannelNumber();
}

bool CPVRChannelGroup::SortAndRenumber()
{
  if (PreventSortAndRenumber())
    return true;

  CSingleLock lock(m_critSection);
  Sort();
  return Renumber();
}

void CPVRChannelGroup::SortByClientChannelNumber()
{
  CSingleLock lock(m_critSection);
  if (!PreventSortAndRenumber())
    std::sort(m_sortedMembers.begin(), m_sortedMembers.end(), sortByClientChannelNumber());
}

void CPVRChannelGroup::SortByChannelNumber()
{
  CSingleLock lock(m_critSection);
  if (!PreventSortAndRenumber())
    std::sort(m_sortedMembers.begin(), m_sortedMembers.end(), sortByChannelNumber());
}

bool CPVRChannelGroup::UpdateClientPriorities()
{
  const std::shared_ptr<CPVRClients> clients = CServiceBroker::GetPVRManager().Clients();
  bool bChanged = false;

  CSingleLock lock(m_critSection);

  for (auto& member : m_sortedMembers)
  {
    int iNewPriority = 0;

    if (m_bUsingBackendChannelOrder)
    {
      std::shared_ptr<CPVRClient> client;
      if (!clients->GetCreatedClient(member->Channel()->ClientID(), client))
        continue;

      iNewPriority = client->GetPriority();
    }
    else
    {
      iNewPriority = 0;
    }

    bChanged |= (member->ClientPriority() != iNewPriority);
    member->SetClientPriority(iNewPriority);
  }

  return bChanged;
}

/********** getters **********/
std::shared_ptr<CPVRChannelGroupMember>& CPVRChannelGroup::GetByUniqueID(
    const std::pair<int, int>& id)
{
  CSingleLock lock(m_critSection);
  const auto it = m_members.find(id);
  return it != m_members.end() ? it->second : CPVRChannelGroup::EmptyMember;
}

const std::shared_ptr<CPVRChannelGroupMember>& CPVRChannelGroup::GetByUniqueID(
    const std::pair<int, int>& id) const
{
  CSingleLock lock(m_critSection);
  const auto it = m_members.find(id);
  return it != m_members.end() ? it->second : CPVRChannelGroup::EmptyMember;
}

std::shared_ptr<CPVRChannel> CPVRChannelGroup::GetByUniqueID(int iUniqueChannelId, int iClientID) const
{
  return GetByUniqueID(std::make_pair(iClientID, iUniqueChannelId))->Channel();
}

std::shared_ptr<CPVRChannel> CPVRChannelGroup::GetByChannelID(int iChannelID) const
{
  CSingleLock lock(m_critSection);

  for (const auto& memberPair : m_members)
  {
    if (memberPair.second->Channel()->ChannelID() == iChannelID)
      return memberPair.second->Channel();
  }

  return {};
}

std::shared_ptr<CPVRChannel> CPVRChannelGroup::GetByChannelEpgID(int iEpgID) const
{
  CSingleLock lock(m_critSection);

  for (const auto& memberPair : m_members)
  {
    if (memberPair.second->Channel()->EpgID() == iEpgID)
      return memberPair.second->Channel();
  }

  return {};
}

std::shared_ptr<CPVRChannel> CPVRChannelGroup::GetLastPlayedChannel(int iCurrentChannel /* = -1 */) const
{
  const std::shared_ptr<CPVRChannelGroupMember> groupMember =
      GetLastPlayedChannelGroupMember(iCurrentChannel);
  return groupMember ? groupMember->Channel() : std::shared_ptr<CPVRChannel>();
}

std::shared_ptr<CPVRChannelGroupMember> CPVRChannelGroup::GetLastPlayedChannelGroupMember(
    int iCurrentChannel) const
{
  CSingleLock lock(m_critSection);

  std::shared_ptr<CPVRChannelGroupMember> groupMember;
  for (const auto& memberPair : m_members)
  {
    const std::shared_ptr<CPVRChannel> channel = memberPair.second->Channel();
    if (channel->ChannelID() != iCurrentChannel &&
        CServiceBroker::GetPVRManager().Clients()->IsCreatedClient(channel->ClientID()) &&
        channel->LastWatched() > 0 &&
        (!groupMember || channel->LastWatched() > groupMember->Channel()->LastWatched()))
    {
      groupMember = memberPair.second;
    }
  }

  return groupMember;
}

CPVRChannelNumber CPVRChannelGroup::GetChannelNumber(const std::shared_ptr<CPVRChannel>& channel) const
{
  CSingleLock lock(m_critSection);
  const std::shared_ptr<CPVRChannelGroupMember>& member = GetByUniqueID(channel->StorageId());
  return member->ChannelNumber();
}

CPVRChannelNumber CPVRChannelGroup::GetClientChannelNumber(const std::shared_ptr<CPVRChannel>& channel) const
{
  CSingleLock lock(m_critSection);
  const std::shared_ptr<CPVRChannelGroupMember>& member = GetByUniqueID(channel->StorageId());
  return member->ClientChannelNumber();
}

std::shared_ptr<CPVRChannel> CPVRChannelGroup::GetByChannelNumber(const CPVRChannelNumber& channelNumber) const
{
  CSingleLock lock(m_critSection);

  for (const auto& member : m_sortedMembers)
  {
    CPVRChannelNumber activeChannelNumber =
        m_bUsingBackendChannelNumbers ? member->ClientChannelNumber() : member->ChannelNumber();
    if (activeChannelNumber == channelNumber)
      return member->Channel();
  }

  return {};
}

std::shared_ptr<CPVRChannel> CPVRChannelGroup::GetNextChannel(const std::shared_ptr<CPVRChannel>& channel) const
{
  std::shared_ptr<CPVRChannel> nextChannel;

  if (channel)
  {
    CSingleLock lock(m_critSection);
    for (auto it = m_sortedMembers.cbegin(); !nextChannel && it != m_sortedMembers.cend(); ++it)
    {
      if ((*it)->Channel() == channel)
      {
        do
        {
          if ((++it) == m_sortedMembers.cend())
            it = m_sortedMembers.cbegin();
          if ((*it)->Channel() && !(*it)->Channel()->IsHidden())
            nextChannel = (*it)->Channel();
        } while (!nextChannel && (*it)->Channel() != channel);

        break;
      }
    }
  }

  return nextChannel;
}

std::shared_ptr<CPVRChannel> CPVRChannelGroup::GetPreviousChannel(const std::shared_ptr<CPVRChannel>& channel) const
{
  std::shared_ptr<CPVRChannel> previousChannel;

  if (channel)
  {
    CSingleLock lock(m_critSection);
    for (auto it = m_sortedMembers.crbegin(); !previousChannel && it != m_sortedMembers.crend();
         ++it)
    {
      if ((*it)->Channel() == channel)
      {
        do
        {
          if ((++it) == m_sortedMembers.crend())
            it = m_sortedMembers.crbegin();
          if ((*it)->Channel() && !(*it)->Channel()->IsHidden())
            previousChannel = (*it)->Channel();
        } while (!previousChannel && (*it)->Channel() != channel);

        break;
      }
    }
  }
  return previousChannel;
}

std::vector<std::shared_ptr<CPVRChannelGroupMember>> CPVRChannelGroup::GetMembers(
    Include eFilter /* = Include::ALL */) const
{
  CSingleLock lock(m_critSection);
  if (eFilter == Include::ALL)
    return m_sortedMembers;

  std::vector<std::shared_ptr<CPVRChannelGroupMember>> members;
  for (const auto& member : m_sortedMembers)
  {
    switch (eFilter)
    {
      case Include::ONLY_HIDDEN:
        if (!member->Channel()->IsHidden())
          continue;
        break;
      case Include::ONLY_VISIBLE:
        if (member->Channel()->IsHidden())
          continue;
       break;
      default:
        break;
    }

    members.emplace_back(member);
  }

  return members;
}

void CPVRChannelGroup::GetChannelNumbers(std::vector<std::string>& channelNumbers) const
{
  CSingleLock lock(m_critSection);
  for (const auto& member : m_sortedMembers)
  {
    CPVRChannelNumber activeChannelNumber =
        m_bUsingBackendChannelNumbers ? member->ClientChannelNumber() : member->ChannelNumber();
    channelNumbers.emplace_back(activeChannelNumber.FormattedChannelNumber());
  }
}

int CPVRChannelGroup::LoadFromDb()
{
  const std::shared_ptr<CPVRDatabase> database(CServiceBroker::GetPVRManager().GetTVDatabase());
  if (!database)
    return -1;

  int iChannelCount = Size();

  database->Get(*this, *m_allChannelsGroup);

  return Size() - iChannelCount;
}

bool CPVRChannelGroup::LoadFromClients()
{
  /* get the channels from the backends */
  return CServiceBroker::GetPVRManager().Clients()->GetChannelGroupMembers(this, m_failedClients) ==
         PVR_ERROR_NO_ERROR;
}

bool CPVRChannelGroup::AddAndUpdateChannels(const CPVRChannelGroup& channels, bool bUseBackendChannelNumbers)
{
  bool bReturn(false);

  /* go through the channel list and check for new channels.
     channels will only by updated in CPVRChannelGroupInternal to prevent dupe updates */
  for (const auto& newMemberPair : channels.m_members)
  {
    /* check whether this channel is known in the internal group */
    const std::shared_ptr<CPVRChannelGroupMember>& existingAllChannelsMember =
        m_allChannelsGroup->GetByUniqueID(newMemberPair.first);
    if (!existingAllChannelsMember->Channel())
      continue;

    const std::shared_ptr<CPVRChannelGroupMember>& newMember = newMemberPair.second;
    /* if it's found, add the channel to this group */
    if (!IsGroupMember(existingAllChannelsMember->Channel()))
    {
      AddToGroup(existingAllChannelsMember->Channel(), newMember->ChannelNumber(),
                 newMember->Order(), bUseBackendChannelNumbers, newMember->ClientChannelNumber());

      bReturn = true;
      CLog::LogFC(LOGDEBUG, LOGPVR, "Added {} channel '{}' to group '{}'",
                  IsRadio() ? "radio" : "TV", existingAllChannelsMember->Channel()->ChannelName(),
                  GroupName());
    }
    else
    {
      CSingleLock lock(m_critSection);
      std::shared_ptr<CPVRChannelGroupMember>& existingMember = GetByUniqueID(newMemberPair.first);

      if (existingMember->ClientChannelNumber() != newMember->ClientChannelNumber() ||
          existingMember->Order() != newMember->Order())
      {
        existingMember->SetClientChannelNumber(newMember->ClientChannelNumber());
        existingMember->SetOrder(newMember->Order());
        bReturn = true;
      }

      CLog::LogFC(LOGDEBUG, LOGPVR, "Updated {} channel '{}' in group '{}'",
                  IsRadio() ? "radio" : "TV", existingMember->Channel()->ChannelName(),
                  GroupName());
    }
  }

  SortAndRenumber();

  return bReturn;
}

bool CPVRChannelGroup::HasValidDataFromClient(int iClientId) const
{
  return std::find(m_failedClients.begin(), m_failedClients.end(), iClientId) ==
         m_failedClients.end();
}

bool CPVRChannelGroup::UpdateChannelNumbersFromAllChannelsGroup()
{
  CSingleLock lock(m_critSection);

  bool bChanged = false;

  if (!IsInternalGroup())
  {
    // If we don't sync channel groups make sure the channel numbers are set from
    // the all channels group using the non default renumber call before sorting
    if (Renumber(IGNORE_NUMBERING_FROM_ONE) || SortAndRenumber())
    {
      Persist();
      bChanged = true;
    }
  }

  m_events.Publish(IsInternalGroup() || bChanged ? PVREvent::ChannelGroupInvalidated
                                                 : PVREvent::ChannelGroup);

  return bChanged;
}

std::vector<std::shared_ptr<CPVRChannel>> CPVRChannelGroup::RemoveDeletedChannels(const CPVRChannelGroup& channels)
{
  std::vector<std::shared_ptr<CPVRChannel>> removedChannels;
  CSingleLock lock(m_critSection);

  /* check for deleted channels */
  for (auto it = m_sortedMembers.begin(); it != m_sortedMembers.end();)
  {
    const std::shared_ptr<CPVRChannel> channel = (*it)->Channel();
    if (channels.m_members.find(channel->StorageId()) == channels.m_members.end())
    {
      m_members.erase(channel->StorageId());
      it = m_sortedMembers.erase(it);

      if (HasValidDataFromClient(channel->ClientID()))
      {
        CLog::Log(LOGINFO, "Removed stale {} channel '{}' from group '{}'",
                  IsRadio() ? "radio" : "TV", channel->ChannelName(), GroupName());

        removedChannels.emplace_back(channel);
      }
    }
    else
    {
      ++it;
    }
  }

  return removedChannels;
}

bool CPVRChannelGroup::UpdateGroupEntries(const CPVRChannelGroup& channels, std::vector<std::shared_ptr<CPVRChannel>>& channelsToRemove)
{
  bool bReturn(false);
  bool bChanged(false);
  bool bRemoved(false);

  CSingleLock lock(m_critSection);
  /* sort by client channel number if this is the first time or if SETTING_PVRMANAGER_BACKENDCHANNELORDER is true */
  bool bUseBackendChannelNumbers(m_members.empty() || m_bUsingBackendChannelOrder);

  SetPreventSortAndRenumber(true);
  channelsToRemove = RemoveDeletedChannels(channels);
  bRemoved = !channelsToRemove.empty();
  bChanged = AddAndUpdateChannels(channels, bUseBackendChannelNumbers) || bRemoved;
  SetPreventSortAndRenumber(false);

  bChanged |= UpdateClientPriorities();

  if (bChanged)
  {
    /* renumber to make sure all channels have a channel number.
       new channels were added at the back, so they'll get the highest numbers */
    bool bRenumbered = SortAndRenumber();
    bReturn = Persist();
    m_events.Publish(HasNewChannels() || bRemoved || bRenumbered ? PVREvent::ChannelGroupInvalidated : PVREvent::ChannelGroup);
  }
  else
  {
    bReturn = true;
  }

  return bReturn;
}

bool CPVRChannelGroup::RemoveFromGroup(const std::shared_ptr<CPVRChannel>& channel)
{
  bool bReturn(false);
  CSingleLock lock(m_critSection);

  for (std::vector<std::shared_ptr<CPVRChannelGroupMember>>::iterator it = m_sortedMembers.begin();
       it != m_sortedMembers.end();)
  {
    if (*channel == *((*it)->Channel()))
    {
      //! @todo notify observers
      m_members.erase((*it)->Channel()->StorageId());
      it = m_sortedMembers.erase(it);
      bReturn = true;
      break;
    }
    else
    {
      ++it;
    }
  }

  // no need to renumber if nothing was removed
  if (bReturn)
    Renumber();

  return bReturn;
}

bool CPVRChannelGroup::AddToGroup(const std::shared_ptr<CPVRChannel>& channel, const CPVRChannelNumber& channelNumber, int iOrder, bool bUseBackendChannelNumbers, const CPVRChannelNumber& clientChannelNumber /* = {} */)
{
  bool bReturn(false);
  CSingleLock lock(m_critSection);

  if (!CPVRChannelGroup::IsGroupMember(channel))
  {
    const std::shared_ptr<CPVRChannelGroupMember>& realMember =
        IsInternalGroup() ? GetByUniqueID(channel->StorageId())
                          : m_allChannelsGroup->GetByUniqueID(channel->StorageId());

    if (realMember->Channel())
    {
      unsigned int iChannelNumber = channelNumber.GetChannelNumber();
      if (!channelNumber.IsValid())
        iChannelNumber = realMember->ChannelNumber().GetChannelNumber();

      CPVRChannelNumber clientChannelNumberToUse = clientChannelNumber;
      if (!clientChannelNumber.IsValid())
        clientChannelNumberToUse = realMember->ClientChannelNumber();

      auto newMember = std::make_shared<CPVRChannelGroupMember>(
          realMember->Channel(),
          CPVRChannelNumber(iChannelNumber, channelNumber.GetSubChannelNumber()),
          realMember->ClientPriority(), iOrder, clientChannelNumberToUse);
      m_sortedMembers.emplace_back(newMember);
      m_members.insert(std::make_pair(realMember->Channel()->StorageId(), newMember));

      SortAndRenumber();

      //! @todo notify observers
      bReturn = true;
    }
  }

  return bReturn;
}

bool CPVRChannelGroup::AppendToGroup(const std::shared_ptr<CPVRChannel>& channel)
{
  CSingleLock lock(m_critSection);

  unsigned int channelNumberMax = 0;
  for (const auto& member : m_sortedMembers)
  {
    if (member->ChannelNumber().GetChannelNumber() > channelNumberMax)
      channelNumberMax = member->ChannelNumber().GetChannelNumber();
  }

  return AddToGroup(channel, CPVRChannelNumber(channelNumberMax + 1, 0), 0, false);
}

bool CPVRChannelGroup::IsGroupMember(const std::shared_ptr<CPVRChannel>& channel) const
{
  CSingleLock lock(m_critSection);
  return m_members.find(channel->StorageId()) != m_members.end();
}

bool CPVRChannelGroup::IsGroupMember(int iChannelId) const
{
  CSingleLock lock(m_critSection);

  for (const auto& memberPair : m_members)
  {
    if (iChannelId == memberPair.second->Channel()->ChannelID())
      return true;
  }

  return false;
}

bool CPVRChannelGroup::Persist()
{
  bool bReturn(true);
  const std::shared_ptr<CPVRDatabase> database(CServiceBroker::GetPVRManager().GetTVDatabase());

  CSingleLock lock(m_critSection);

  // do not persist if the group is not fully loaded and was saved before.
  if (!m_bLoaded && m_iGroupId != INVALID_GROUP_ID)
    return bReturn;

  // Mark newly created groups as loaded so future updates will also be persisted...
  if (m_iGroupId == INVALID_GROUP_ID)
    m_bLoaded = true;

  if (database)
  {
    CLog::LogFC(LOGDEBUG, LOGPVR, "Persisting channel group '{}' with {} channels", GroupName(),
                static_cast<int>(m_members.size()));

    bReturn = database->Persist(*this);
    m_bChanged = false;
  }
  else
  {
    bReturn = false;
  }

  return bReturn;
}

bool CPVRChannelGroup::Renumber(RenumberMode mode /* = NORMAL */)
{
  if (PreventSortAndRenumber())
    return true;

  bool bReturn(false);
  unsigned int iChannelNumber(0);
  const auto& settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  bool bUsingBackendChannelNumbers = UsingBackendChannelNumbers(settings);
  bool bStartGroupChannelNumbersFromOne =
      settings->GetBool(CSettings::SETTING_PVRMANAGER_STARTGROUPCHANNELNUMBERSFROMONE) &&
      !bUsingBackendChannelNumbers;

  CSingleLock lock(m_critSection);

  CPVRChannelNumber currentChannelNumber;
  CPVRChannelNumber currentClientChannelNumber;
  for (auto& sortedMember : m_sortedMembers)
  {
    currentClientChannelNumber = sortedMember->ClientChannelNumber();
    if (!currentClientChannelNumber.IsValid())
      currentClientChannelNumber =
          m_allChannelsGroup->GetClientChannelNumber(sortedMember->Channel());

    if (bUsingBackendChannelNumbers)
    {
      currentChannelNumber = currentClientChannelNumber;
    }
    else if (sortedMember->Channel()->IsHidden())
    {
      currentChannelNumber = CPVRChannelNumber(0, 0);
    }
    else
    {
      if (IsInternalGroup())
      {
        currentChannelNumber = CPVRChannelNumber(++iChannelNumber, 0);
      }
      else
      {
        if (bStartGroupChannelNumbersFromOne && mode != IGNORE_NUMBERING_FROM_ONE)
          currentChannelNumber = CPVRChannelNumber(++iChannelNumber, 0);
        else
          currentChannelNumber = m_allChannelsGroup->GetChannelNumber(sortedMember->Channel());
      }
    }

    if (sortedMember->ChannelNumber() != currentChannelNumber ||
        sortedMember->ClientChannelNumber() != currentClientChannelNumber)
    {
      bReturn = true;
      sortedMember->SetChannelNumber(currentChannelNumber);
      sortedMember->SetClientChannelNumber(currentClientChannelNumber);

      auto& unsortedMember = GetByUniqueID(sortedMember->Channel()->StorageId());
      unsortedMember->SetChannelNumber(sortedMember->ChannelNumber());
      unsortedMember->SetClientChannelNumber(sortedMember->ClientChannelNumber());
    }
  }

  Sort();

  return bReturn;
}

bool CPVRChannelGroup::HasNewChannels() const
{
  CSingleLock lock(m_critSection);

  for (const auto& memberPair : m_members)
  {
    if (memberPair.second->Channel()->ChannelID() <= 0)
      return true;
  }

  return false;
}

bool CPVRChannelGroup::HasChanges() const
{
  CSingleLock lock(m_critSection);
  return m_bChanged;
}

bool CPVRChannelGroup::IsNew() const
{
  CSingleLock lock(m_critSection);
  return m_iGroupId <= 0;
}

void CPVRChannelGroup::OnSettingChanged(const std::shared_ptr<const CSetting>& setting)
{
  if (setting == NULL)
    return;

  //! @todo while pvr manager is starting up do accept setting changes.
  if(!CServiceBroker::GetPVRManager().IsStarted())
  {
    CLog::Log(LOGWARNING, "Channel group setting change ignored while PVR Manager is starting");
    return;
  }

  const std::string& settingId = setting->GetId();
  if (settingId == CSettings::SETTING_PVRMANAGER_SYNCCHANNELGROUPS ||
      settingId == CSettings::SETTING_PVRMANAGER_BACKENDCHANNELORDER ||
      settingId == CSettings::SETTING_PVRMANAGER_USEBACKENDCHANNELNUMBERS ||
      settingId == CSettings::SETTING_PVRMANAGER_STARTGROUPCHANNELNUMBERSFROMONE)
  {
    const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();
    m_bSyncChannelGroups = settings->GetBool(CSettings::SETTING_PVRMANAGER_SYNCCHANNELGROUPS);
    bool bUsingBackendChannelOrder = settings->GetBool(CSettings::SETTING_PVRMANAGER_BACKENDCHANNELORDER);
    bool bUsingBackendChannelNumbers = UsingBackendChannelNumbers(settings);
    bool bStartGroupChannelNumbersFromOne = settings->GetBool(CSettings::SETTING_PVRMANAGER_STARTGROUPCHANNELNUMBERSFROMONE) && !bUsingBackendChannelNumbers;

    CSingleLock lock(m_critSection);

    bool bChannelNumbersChanged = m_bUsingBackendChannelNumbers != bUsingBackendChannelNumbers;
    bool bChannelOrderChanged = m_bUsingBackendChannelOrder != bUsingBackendChannelOrder;
    bool bGroupChannelNumbersFromOneChanged = m_bStartGroupChannelNumbersFromOne != bStartGroupChannelNumbersFromOne;

    m_bUsingBackendChannelOrder = bUsingBackendChannelOrder;
    m_bUsingBackendChannelNumbers = bUsingBackendChannelNumbers;
    m_bStartGroupChannelNumbersFromOne = bStartGroupChannelNumbersFromOne;

    /* check whether this channel group has to be renumbered */
    if (bChannelOrderChanged || bChannelNumbersChanged || bGroupChannelNumbersFromOneChanged)
    {
      CLog::LogFC(LOGDEBUG, LOGPVR,
                  "Renumbering channel group '{}' to use the backend channel order and/or numbers",
                  GroupName());

      if (bChannelOrderChanged)
        UpdateClientPriorities();

      // If we don't sync channel groups make sure the channel numbers are set from
      // the all channels group using the non default renumber call before sorting
      if (!m_bSyncChannelGroups)
        Renumber(IGNORE_NUMBERING_FROM_ONE);

      bool bRenumbered = SortAndRenumber();
      Persist();

      m_events.Publish(bRenumbered ? PVREvent::ChannelGroupInvalidated : PVREvent::ChannelGroup);
    }
  }
}

CDateTime CPVRChannelGroup::GetEPGDate(EpgDateType epgDateType) const
{
  CDateTime date;
  std::shared_ptr<CPVREpg> epg;
  std::shared_ptr<CPVRChannel> channel;
  CSingleLock lock(m_critSection);

  for (const auto& memberPair : m_members)
  {
    channel = memberPair.second->Channel();
    if (!channel->IsHidden() && (epg = channel->GetEPG()))
    {
      CDateTime epgDate;
      switch (epgDateType)
      {
        case EPG_FIRST_DATE:
          epgDate = epg->GetFirstDate();
          if (epgDate.IsValid() && (!date.IsValid() || epgDate < date))
            date = epgDate;
          break;

        case EPG_LAST_DATE:
          epgDate = epg->GetLastDate();
          if (epgDate.IsValid() && (!date.IsValid() || epgDate > date))
            date = epgDate;
          break;
      }
    }
  }

  return date;
}

CDateTime CPVRChannelGroup::GetFirstEPGDate() const
{
  return GetEPGDate(EPG_FIRST_DATE);
}

CDateTime CPVRChannelGroup::GetLastEPGDate() const
{
  return GetEPGDate(EPG_LAST_DATE);
}

int CPVRChannelGroup::GroupID() const
{
  return m_iGroupId;
}

void CPVRChannelGroup::SetGroupID(int iGroupId)
{
  if (iGroupId >= 0)
    m_iGroupId = iGroupId;
}

void CPVRChannelGroup::SetGroupType(int iGroupType)
{
  CSingleLock lock(m_critSection);
  if (m_iGroupType != iGroupType)
  {
    m_iGroupType = iGroupType;
    if (m_bLoaded)
      m_bChanged = true;
  }
}

int CPVRChannelGroup::GroupType() const
{
  return m_iGroupType;
}

std::string CPVRChannelGroup::GroupName() const
{
  CSingleLock lock(m_critSection);
  return m_path.GetGroupName();
}

void CPVRChannelGroup::SetGroupName(const std::string& strGroupName)
{
  CSingleLock lock(m_critSection);
  if (m_path.GetGroupName() != strGroupName)
  {
    m_path = CPVRChannelsPath(m_path.IsRadio(), strGroupName);
    if (m_bLoaded)
    {
      m_bChanged = true;
      Persist(); //! @todo why must we persist immediately?
    }
  }
}

bool CPVRChannelGroup::IsRadio() const
{
  CSingleLock lock(m_critSection);
  return m_path.IsRadio();
}

time_t CPVRChannelGroup::LastWatched() const
{
  CSingleLock lock(m_critSection);
  return m_iLastWatched;
}

void CPVRChannelGroup::SetLastWatched(time_t iLastWatched)
{
  const std::shared_ptr<CPVRDatabase> database(CServiceBroker::GetPVRManager().GetTVDatabase());

  CSingleLock lock(m_critSection);

  if (m_iLastWatched != iLastWatched)
  {
    m_iLastWatched = iLastWatched;
    if (m_bLoaded && database)
      database->UpdateLastWatched(*this);
  }
}

uint64_t CPVRChannelGroup::LastOpened() const
{
  CSingleLock lock(m_critSection);
  return m_iLastOpened;
}

void CPVRChannelGroup::SetLastOpened(uint64_t iLastOpened)
{
  const std::shared_ptr<CPVRDatabase> database(CServiceBroker::GetPVRManager().GetTVDatabase());

  CSingleLock lock(m_critSection);

  if (m_iLastOpened != iLastOpened)
  {
    m_iLastOpened = iLastOpened;
    if (m_bLoaded && database)
      database->UpdateLastOpened(*this);
  }
}

bool CPVRChannelGroup::PreventSortAndRenumber() const
{
  CSingleLock lock(m_critSection);
  return m_bPreventSortAndRenumber;
}

void CPVRChannelGroup::SetPreventSortAndRenumber(bool bPreventSortAndRenumber /* = true */)
{
  CSingleLock lock(m_critSection);
  m_bPreventSortAndRenumber = bPreventSortAndRenumber;
}

bool CPVRChannelGroup::UpdateChannel(const std::pair<int, int>& storageId,
                                     const std::string& strChannelName,
                                     const std::string& strIconPath,
                                     int iEPGSource,
                                     int iChannelNumber,
                                     bool bHidden,
                                     bool bEPGEnabled,
                                     bool bParentalLocked,
                                     bool bUserSetIcon)
{
  CSingleLock lock(m_critSection);

  /* get the real channel from the group */
  const std::shared_ptr<CPVRChannel> channel = GetByUniqueID(storageId)->Channel();
  if (!channel)
    return false;

  channel->SetChannelName(strChannelName, true);
  channel->SetHidden(bHidden);
  channel->SetLocked(bParentalLocked);
  channel->SetIconPath(strIconPath, bUserSetIcon);

  if (iEPGSource == 0)
    channel->SetEPGScraper("client");

  //! @todo add other scrapers
  channel->SetEPGEnabled(bEPGEnabled);

  /* set new values in the channel tag */
  if (bHidden)
  {
    // sort or previous changes will be overwritten
    Sort();

    RemoveFromGroup(channel);
  }
  else
  {
    SetChannelNumber(channel, CPVRChannelNumber(iChannelNumber, 0));
  }

  return true;
}

size_t CPVRChannelGroup::Size() const
{
  CSingleLock lock(m_critSection);
  return m_members.size();
}

bool CPVRChannelGroup::HasChannels() const
{
  CSingleLock lock(m_critSection);
  return !m_members.empty();
}

bool CPVRChannelGroup::CreateChannelEpgs(bool bForce /* = false */)
{
  /* used only by internal channel groups */
  return true;
}

bool CPVRChannelGroup::SetHidden(bool bHidden)
{
  CSingleLock lock(m_critSection);

  if (m_bHidden != bHidden)
  {
    m_bHidden = bHidden;
    if (m_bLoaded)
      m_bChanged = true;
  }

  return m_bChanged;
}

bool CPVRChannelGroup::IsHidden() const
{
  CSingleLock lock(m_critSection);
  return m_bHidden;
}

int CPVRChannelGroup::GetPosition() const
{
  CSingleLock lock(m_critSection);
  return m_iPosition;
}

void CPVRChannelGroup::SetPosition(int iPosition)
{
  CSingleLock lock(m_critSection);

  if (m_iPosition != iPosition)
  {
    m_iPosition = iPosition;
    if (m_bLoaded)
      m_bChanged = true;
  }
}
