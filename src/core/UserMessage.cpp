/*
 *  This file is part of CounterStrikeSharp.
 *  CounterStrikeSharp is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  CounterStrikeSharp is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with CounterStrikeSharp.  If not, see <https://www.gnu.org/licenses/>. *
 */
#include "UserMessage.h"

#include "networksystem/inetworkserializer.h"

using namespace google;

namespace counterstrikesharp {
int UserMessage::GetMessageID() { return msgSerializable->GetNetMessageInfo()->m_MessageId; }

const std::string& UserMessage::GetMessageName()
{
    if (!displayName.empty() || !msgSerializable) return displayName;

    displayName = msgSerializable->GetUnscopedName();

    auto open = displayName.rfind(" [");
    if (open != std::string::npos && displayName.size() > open + 3 && displayName.back() == ']'
        && displayName.find_first_not_of("-0123456789", open + 2) == displayName.size() - 1)
    {
        displayName.erase(open);
    }

    return displayName;
}

bool UserMessage::HasField(std::string fieldName)
{
    const google::protobuf::Descriptor* descriptor = msg->GetDescriptor();
    const google::protobuf::FieldDescriptor* field = descriptor->FindFieldByName(fieldName);

    if (field == nullptr || (field->label() == google::protobuf::FieldDescriptor::LABEL_REPEATED))
    {
        return false;
    }

    return this->msg->GetReflection()->HasField(*this->msg, field);
}

const CNetMessagePB<google::protobuf::Message>* UserMessage::GetProtobufMessage() { return msg; }
} // namespace counterstrikesharp
