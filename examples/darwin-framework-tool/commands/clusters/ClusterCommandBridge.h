/*
 *   Copyright (c) 2022 Project CHIP Authors
 *   All rights reserved.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 *
 */

#pragma once

#import "MTRError_Utils.h"
#import <Matter/Matter.h>

#include "ModelCommandBridge.h"

class ClusterCommand : public ModelCommand {
public:
    ClusterCommand()
        : ModelCommand("command-by-id")
    {
        AddArgument("cluster-id", 0, UINT32_MAX, &mClusterId);
        AddArgument("command-id", 0, UINT32_MAX, &mCommandId);
        AddArgument("payload", &mPayload);
        AddArguments();
    }

    ClusterCommand(chip::ClusterId clusterId)
        : ModelCommand("command-by-id")
        , mClusterId(clusterId)
    {
        AddArgument("command-id", 0, UINT32_MAX, &mCommandId);
        AddArgument("payload", &mPayload);
        AddArguments();
    }

    ~ClusterCommand() {}

    using ModelCommand::SendCommand;

    CHIP_ERROR SendCommand(MTRBaseDevice * _Nonnull device, chip::EndpointId endpointId) override
    {
        id commandFields;
        ReturnErrorOnFailure(GetCommandFields(&commandFields));
        return ClusterCommand::SendCommand(device, endpointId, mClusterId, mCommandId, commandFields);
    }

    CHIP_ERROR SendCommand(MTRBaseDevice * _Nonnull device, chip::EndpointId endpointId, chip::ClusterId clusterId,
        chip::CommandId commandId, id _Nonnull commandFields)
    {
        uint16_t repeatCount = mRepeatCount.ValueOr(1);
        uint16_t __block responsesNeeded = repeatCount;
        dispatch_queue_t callbackQueue = dispatch_queue_create("com.chip.command", DISPATCH_QUEUE_SERIAL_WITH_AUTORELEASE_POOL);

        __auto_type * endpoint = @(endpointId);
        __auto_type * cluster = @(clusterId);
        __auto_type * command = @(commandId);
        while (repeatCount--) {
            [device invokeCommandWithEndpointID:endpoint
                                      clusterID:cluster
                                      commandID:command
                                  commandFields:commandFields
                             timedInvokeTimeout:mTimedInteractionTimeoutMs.HasValue()
                                 ? [NSNumber numberWithUnsignedShort:mTimedInteractionTimeoutMs.Value()]
                                 : nil
                                          queue:callbackQueue
                                     completion:^(
                                         NSArray<NSDictionary<NSString *, id> *> * _Nullable values, NSError * _Nullable error) {
                                         responsesNeeded--;
                                         if (error != nil) {
                                             mError = error;
                                             LogNSError("Error", error);
                                             RemoteDataModelLogger::LogCommandErrorAsJSON(endpoint, cluster, command, error);
                                         } else {
                                             RemoteDataModelLogger::LogCommandAsJSON(endpoint, cluster, command, values);
                                         }
                                         if (responsesNeeded == 0) {
                                             SetCommandExitStatus(mError);
                                         }
                                     }];

            if (mRepeatDelayInMs.HasValue()) {
                [NSThread sleepForTimeInterval:((double) mRepeatDelayInMs.Value()) / 1000];
            }
        }
        return CHIP_NO_ERROR;
    }

    void Shutdown() override
    {
        mError = nil;
        ModelCommand::Shutdown();
    }

protected:
    ClusterCommand(const char * _Nonnull commandName)
        : ModelCommand(commandName)
    {
        // Subclasses are responsible for calling AddArguments.
    }

    void AddArguments()
    {
        AddArgument("timedInteractionTimeoutMs", 0, UINT16_MAX, &mTimedInteractionTimeoutMs,
            "If provided, do a timed invoke with the given timed interaction timeout. See \"7.6.10. Timed Interaction\" in the "
            "Matter specification.");
        AddArgument("repeat-count", 1, UINT16_MAX, &mRepeatCount);
        AddArgument("repeat-delay-ms", 0, UINT16_MAX, &mRepeatDelayInMs);
        ModelCommand::AddArguments();
    }

    chip::Optional<uint16_t> mTimedInteractionTimeoutMs;
    chip::Optional<uint16_t> mRepeatCount;
    chip::Optional<uint16_t> mRepeatDelayInMs;
    NSError * _Nullable mError = nil;

private:
    CHIP_ERROR GetCommandFields(id _Nonnull * _Nonnull outCommandFields)
    {
        CHIP_ERROR err = CHIP_NO_ERROR;
        chip::TLV::TLVWriter writer;
        chip::TLV::TLVReader reader;

        mData = static_cast<uint8_t *>(chip::Platform::MemoryCalloc(sizeof(uint8_t), mDataMaxLen));
        VerifyOrExit(mData != nullptr, err = CHIP_ERROR_NO_MEMORY);

        writer.Init(mData, mDataMaxLen);

        err = mPayload.Encode(writer, chip::TLV::AnonymousTag());
        SuccessOrExit(err);

        reader.Init(mData, writer.GetLengthWritten());
        err = reader.Next();
        SuccessOrExit(err);

        *outCommandFields = NSObjectFromCHIPTLV(&reader);
        VerifyOrDo(nil != *outCommandFields, err = CHIP_ERROR_INTERNAL);

    exit:
        if (nullptr != mData) {
            chip::Platform::MemoryFree(mData);
            mData = nullptr;
        }
        return err;
    }

    chip::ClusterId mClusterId;
    chip::CommandId mCommandId;

    CustomArgument mPayload;
    static constexpr uint32_t mDataMaxLen = 4096;
    uint8_t * _Nullable mData = nullptr;
};
