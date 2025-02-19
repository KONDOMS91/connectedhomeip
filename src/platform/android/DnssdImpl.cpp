/*
 *
 *    Copyright (c) 2020-2021 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "DnssdImpl.h"

#include <lib/dnssd/platform/Dnssd.h>
#include <lib/support/CHIPJNIError.h>
#include <lib/support/CHIPMemString.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/JniReferences.h>
#include <lib/support/JniTypeWrappers.h>
#include <lib/support/SafeInt.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/internal/CHIPDeviceLayerInternal.h>

#include <cstddef>
#include <jni.h>
#include <memory>
#include <sstream>
#include <string>

namespace chip {
namespace Dnssd {

using namespace chip::Platform;

namespace {
JniGlobalReference sResolverObject;
JniGlobalReference sBrowserObject;
JniGlobalReference sMdnsCallbackObject;
jmethodID sResolveMethod          = nullptr;
jmethodID sBrowseMethod           = nullptr;
jmethodID sStopBrowseMethod       = nullptr;
jmethodID sGetTextEntryKeysMethod = nullptr;
jmethodID sGetTextEntryDataMethod = nullptr;
jclass sMdnsCallbackClass         = nullptr;
jmethodID sPublishMethod          = nullptr;
jmethodID sRemoveServicesMethod   = nullptr;
} // namespace

// Implementation of functions declared in lib/dnssd/platform/Dnssd.h

constexpr char kProtocolTcp[] = "._tcp";
constexpr char kProtocolUdp[] = "._udp";

CHIP_ERROR ChipDnssdInit(DnssdAsyncReturnCallback initCallback, DnssdAsyncReturnCallback errorCallback, void * context)
{
    VerifyOrReturnError(initCallback != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(errorCallback != nullptr, CHIP_ERROR_INVALID_ARGUMENT);

    initCallback(context, CHIP_NO_ERROR);
    return CHIP_NO_ERROR;
}

void ChipDnssdShutdown() {}

CHIP_ERROR ChipDnssdRemoveServices()
{

    VerifyOrReturnError(sResolverObject.HasValidObjectRef() && sRemoveServicesMethod != nullptr, CHIP_ERROR_INCORRECT_STATE);
    JNIEnv * env = JniReferences::GetInstance().GetEnvForCurrentThread();

    {
        DeviceLayer::StackUnlock unlock;
        env->CallVoidMethod(sResolverObject.ObjectRef(), sRemoveServicesMethod);
    }

    if (env->ExceptionCheck())
    {
        ChipLogError(Discovery, "Java exception in ChipDnssdRemoveServices");
        env->ExceptionDescribe();
        env->ExceptionClear();
        return CHIP_JNI_ERROR_EXCEPTION_THROWN;
    }

    return CHIP_NO_ERROR;
}

CHIP_ERROR ChipDnssdPublishService(const DnssdService * service, DnssdPublishCallback callback, void * context)
{
    VerifyOrReturnError(service != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(sResolverObject.HasValidObjectRef() && sPublishMethod != nullptr, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(CanCastTo<uint32_t>(service->mTextEntrySize), CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(CanCastTo<uint32_t>(service->mSubTypeSize), CHIP_ERROR_INVALID_ARGUMENT);

    JNIEnv * env = JniReferences::GetInstance().GetEnvForCurrentThread();
    UtfString jniName(env, service->mName);
    UtfString jniHostName(env, service->mHostName);

    std::string serviceType = service->mType;
    serviceType += '.';
    serviceType += (service->mProtocol == DnssdServiceProtocol::kDnssdProtocolUdp ? "_udp" : "_tcp");
    UtfString jniServiceType(env, serviceType.c_str());

    auto textEntrySize = static_cast<uint32_t>(service->mTextEntrySize);

    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray keys  = env->NewObjectArray(textEntrySize, stringClass, nullptr);

    jclass arrayElemType = env->FindClass("[B");
    jobjectArray datas   = env->NewObjectArray(textEntrySize, arrayElemType, nullptr);

    for (uint32_t i = 0; i < textEntrySize; i++)
    {
        UtfString jniKey(env, service->mTextEntries[i].mKey);
        env->SetObjectArrayElement(keys, i, jniKey.jniValue());

        VerifyOrReturnError(CanCastTo<uint32_t>(service->mTextEntries[i].mDataSize), CHIP_ERROR_INVALID_ARGUMENT);
        auto dataSize = static_cast<uint32_t>(service->mTextEntries[i].mDataSize);
        ByteArray jniData(env, (const jbyte *) service->mTextEntries[i].mData, dataSize);
        env->SetObjectArrayElement(datas, i, jniData.jniValue());
    }

    auto subTypeSize      = static_cast<uint32_t>(service->mSubTypeSize);
    jobjectArray subTypes = env->NewObjectArray(subTypeSize, stringClass, nullptr);
    for (uint32_t i = 0; i < subTypeSize; i++)
    {
        UtfString jniSubType(env, service->mSubTypes[i]);
        env->SetObjectArrayElement(subTypes, i, jniSubType.jniValue());
    }

    {
        DeviceLayer::StackUnlock unlock;
        env->CallVoidMethod(sResolverObject.ObjectRef(), sPublishMethod, jniName.jniValue(), jniHostName.jniValue(),
                            jniServiceType.jniValue(), service->mPort, keys, datas, subTypes);
    }

    if (env->ExceptionCheck())
    {
        ChipLogError(Discovery, "Java exception in ChipDnssdPublishService");
        env->ExceptionDescribe();
        env->ExceptionClear();
        return CHIP_JNI_ERROR_EXCEPTION_THROWN;
    }

    return CHIP_NO_ERROR;
}

CHIP_ERROR ChipDnssdFinalizeServiceUpdate()
{
    return CHIP_NO_ERROR;
}

std::string GetFullType(const char * type, DnssdServiceProtocol protocol)
{
    std::ostringstream typeBuilder;
    typeBuilder << type;
    typeBuilder << (protocol == DnssdServiceProtocol::kDnssdProtocolUdp ? kProtocolUdp : kProtocolTcp);
    return typeBuilder.str();
}

std::string GetFullTypeWithSubTypes(const char * type, DnssdServiceProtocol protocol)
{
    auto fullType = GetFullType(type, protocol);

    std::string subtypeDelimiter = "._sub.";
    size_t position              = fullType.find(subtypeDelimiter);
    if (position != std::string::npos)
    {
        fullType = fullType.substr(position + subtypeDelimiter.size()) + "," + fullType.substr(0, position);
    }

    return fullType;
}

std::string GetFullType(const DnssdService * service)
{
    return GetFullType(service->mType, service->mProtocol);
}

CHIP_ERROR ChipDnssdBrowse(const char * type, DnssdServiceProtocol protocol, Inet::IPAddressType addressType,
                           Inet::InterfaceId interface, DnssdBrowseCallback callback, void * context, intptr_t * browseIdentifier)
{
    VerifyOrReturnError(type != nullptr && callback != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(sBrowserObject.HasValidObjectRef() && sBrowseMethod != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(sMdnsCallbackObject.HasValidObjectRef(), CHIP_ERROR_INCORRECT_STATE);

    std::string serviceType = GetFullTypeWithSubTypes(type, protocol);
    JNIEnv * env            = JniReferences::GetInstance().GetEnvForCurrentThread();
    VerifyOrReturnError(env != nullptr, CHIP_JNI_ERROR_NO_ENV,
                        ChipLogError(Discovery, "Failed to GetEnvForCurrentThread for ChipDnssdBrowse"));
    UtfString jniServiceType(env, serviceType.c_str());

    env->CallVoidMethod(sBrowserObject.ObjectRef(), sBrowseMethod, jniServiceType.jniValue(), reinterpret_cast<jlong>(callback),
                        reinterpret_cast<jlong>(context), sMdnsCallbackObject.ObjectRef());

    if (env->ExceptionCheck())
    {
        ChipLogError(Discovery, "Java exception in ChipDnssdBrowse");
        env->ExceptionDescribe();
        env->ExceptionClear();
        return CHIP_JNI_ERROR_EXCEPTION_THROWN;
    }

    auto sdCtx = chip::Platform::New<BrowseContext>(callback);
    VerifyOrReturnError(nullptr != sdCtx, CHIP_ERROR_NO_MEMORY,
                        ChipLogError(Discovery, "Failed allocate memory for BrowseContext in ChipDnssdBrowse"));
    *browseIdentifier = reinterpret_cast<intptr_t>(sdCtx);

    return CHIP_NO_ERROR;
}

CHIP_ERROR ChipDnssdStopBrowse(intptr_t browseIdentifier)
{
    VerifyOrReturnError(browseIdentifier != 0, CHIP_ERROR_INVALID_ARGUMENT,
                        ChipLogError(Discovery, "ChipDnssdStopBrowse Invalid argument browseIdentifier = 0"));
    VerifyOrReturnError(sBrowserObject.HasValidObjectRef() && sStopBrowseMethod != nullptr, CHIP_ERROR_INVALID_ARGUMENT);

    JNIEnv * env = JniReferences::GetInstance().GetEnvForCurrentThread();
    VerifyOrReturnError(env != nullptr, CHIP_JNI_ERROR_NO_ENV,
                        ChipLogError(Discovery, "Failed to GetEnvForCurrentThread for ChipDnssdStopBrowse"));
    auto ctx = reinterpret_cast<BrowseContext *>(browseIdentifier);

    env->CallVoidMethod(sBrowserObject.ObjectRef(), sStopBrowseMethod, reinterpret_cast<jlong>(ctx->callback));

    chip::Platform::Delete(ctx);
    ctx = nullptr;
    if (env->ExceptionCheck())
    {
        ChipLogError(Discovery, "Java exception in ChipDnssdStopBrowse");
        env->ExceptionDescribe();
        env->ExceptionClear();
        return CHIP_JNI_ERROR_EXCEPTION_THROWN;
    }

    return CHIP_NO_ERROR;
}

template <size_t N>
CHIP_ERROR extractProtocol(const char * serviceType, char (&outServiceName)[N], DnssdServiceProtocol & outProtocol)
{
    const char * dotPos = strrchr(serviceType, '.');
    VerifyOrReturnError(dotPos != nullptr, CHIP_ERROR_INVALID_ARGUMENT);

    size_t lengthWithoutProtocol = static_cast<size_t>(dotPos - serviceType);
    VerifyOrReturnError(lengthWithoutProtocol + 1 <= N, CHIP_ERROR_INVALID_ARGUMENT);

    memcpy(outServiceName, serviceType, lengthWithoutProtocol);
    outServiceName[lengthWithoutProtocol] = '\0'; // Set a null terminator

    outProtocol = DnssdServiceProtocol::kDnssdProtocolUnknown;
    if (strstr(dotPos, "._tcp") != 0)
    {
        outProtocol = DnssdServiceProtocol::kDnssdProtocolTcp;
    }
    else if (strstr(dotPos, "._udp") != 0)
    {
        outProtocol = DnssdServiceProtocol::kDnssdProtocolUdp;
    }
    else
    {
        ChipLogError(Discovery, "protocol type don't include neithor TCP nor UDP!");
        return CHIP_ERROR_INVALID_ARGUMENT;
    }

    VerifyOrReturnError(outProtocol != DnssdServiceProtocol::kDnssdProtocolUnknown, CHIP_ERROR_INVALID_ARGUMENT);

    return CHIP_NO_ERROR;
}

CHIP_ERROR ChipDnssdResolve(DnssdService * service, Inet::InterfaceId interface, DnssdResolveCallback callback, void * context)
{
    VerifyOrReturnError(service != nullptr && callback != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(sResolverObject.HasValidObjectRef() && sResolveMethod != nullptr, CHIP_ERROR_INCORRECT_STATE);
    VerifyOrReturnError(sMdnsCallbackObject.HasValidObjectRef(), CHIP_ERROR_INCORRECT_STATE);

    std::string serviceType = GetFullType(service);
    JNIEnv * env            = JniReferences::GetInstance().GetEnvForCurrentThread();
    UtfString jniInstanceName(env, service->mName);
    UtfString jniServiceType(env, serviceType.c_str());

    {
        DeviceLayer::StackUnlock unlock;
        env->CallVoidMethod(sResolverObject.ObjectRef(), sResolveMethod, jniInstanceName.jniValue(), jniServiceType.jniValue(),
                            reinterpret_cast<jlong>(callback), reinterpret_cast<jlong>(context), sMdnsCallbackObject.ObjectRef());
    }

    if (env->ExceptionCheck())
    {
        ChipLogError(Discovery, "Java exception in ChipDnssdResolve");
        env->ExceptionDescribe();
        env->ExceptionClear();
        return CHIP_JNI_ERROR_EXCEPTION_THROWN;
    }

    return CHIP_NO_ERROR;
}

void ChipDnssdResolveNoLongerNeeded(const char * instanceName) {}

CHIP_ERROR ChipDnssdReconfirmRecord(const char * hostname, chip::Inet::IPAddress address, chip::Inet::InterfaceId interface)
{
    return CHIP_ERROR_NOT_IMPLEMENTED;
}

// Implemention of Java-specific functions

void InitializeWithObjects(jobject resolverObject, jobject browserObject, jobject mdnsCallbackObject)
{
    JNIEnv * env = JniReferences::GetInstance().GetEnvForCurrentThread();
    VerifyOrReturn(sResolverObject.Init(resolverObject) == CHIP_NO_ERROR,
                   ChipLogError(Discovery, "Failed to init sResolverObject"));
    VerifyOrReturn(sBrowserObject.Init(browserObject) == CHIP_NO_ERROR, ChipLogError(Discovery, "Failed to init sBrowserObject"));
    VerifyOrReturn(sMdnsCallbackObject.Init(mdnsCallbackObject) == CHIP_NO_ERROR,
                   ChipLogError(Discovery, "Failed to init sMdnsCallbackObject"));

    jclass resolverClass = env->GetObjectClass(resolverObject);
    jclass browserClass  = env->GetObjectClass(browserObject);
    sMdnsCallbackClass   = env->GetObjectClass(mdnsCallbackObject);

    VerifyOrReturn(browserClass != nullptr, ChipLogError(Discovery, "Failed to get Browse Java class"));
    VerifyOrReturn(resolverClass != nullptr, ChipLogError(Discovery, "Failed to get Resolver Java class"));

    sGetTextEntryKeysMethod = env->GetMethodID(sMdnsCallbackClass, "getTextEntryKeys", "(Ljava/util/Map;)[Ljava/lang/String;");

    sGetTextEntryDataMethod = env->GetMethodID(sMdnsCallbackClass, "getTextEntryData", "(Ljava/util/Map;Ljava/lang/String;)[B");

    sResolveMethod =
        env->GetMethodID(resolverClass, "resolve", "(Ljava/lang/String;Ljava/lang/String;JJLchip/platform/ChipMdnsCallback;)V");

    sBrowseMethod = env->GetMethodID(browserClass, "browse", "(Ljava/lang/String;JJLchip/platform/ChipMdnsCallback;)V");

    sStopBrowseMethod = env->GetMethodID(browserClass, "stopDiscover", "(J)V");

    if (sResolveMethod == nullptr)
    {
        ChipLogError(Discovery, "Failed to access Resolver 'resolve' method");
        env->ExceptionClear();
    }

    if (sBrowseMethod == nullptr)
    {
        ChipLogError(Discovery, "Failed to access Discover 'browse' method");
        env->ExceptionClear();
    }

    if (sStopBrowseMethod == nullptr)
    {
        ChipLogError(Discovery, "Failed to access Discover 'stopDiscover' method");
        env->ExceptionClear();
    }

    if (sGetTextEntryKeysMethod == nullptr)
    {
        ChipLogError(Discovery, "Failed to access MdnsCallback 'getTextEntryKeys' method");
        env->ExceptionClear();
    }

    if (sGetTextEntryDataMethod == nullptr)
    {
        ChipLogError(Discovery, "Failed to access MdnsCallback 'getTextEntryData' method");
        env->ExceptionClear();
    }

    sPublishMethod =
        env->GetMethodID(resolverClass, "publish",
                         "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I[Ljava/lang/String;[[B[Ljava/lang/String;)V");
    if (sPublishMethod == nullptr)
    {
        ChipLogError(Discovery, "Failed to access Resolver 'publish' method");
        env->ExceptionClear();
    }

    sRemoveServicesMethod = env->GetMethodID(resolverClass, "removeServices", "()V");
    if (sRemoveServicesMethod == nullptr)
    {
        ChipLogError(Discovery, "Failed to access Resolver 'removeServices' method");
        env->ExceptionClear();
    }
}

void HandleResolve(jstring instanceName, jstring serviceType, jstring hostName, jstring address, jint port, jobject textEntries,
                   jlong callbackHandle, jlong contextHandle)
{
    VerifyOrReturn(callbackHandle != 0, ChipLogError(Discovery, "HandleResolve called with callback equal to nullptr"));

    const auto dispatch = [callbackHandle, contextHandle](CHIP_ERROR error, DnssdService * service = nullptr,
                                                          Inet::IPAddress * address = nullptr) {
        DeviceLayer::StackLock lock;
        DnssdResolveCallback callback = reinterpret_cast<DnssdResolveCallback>(callbackHandle);
        size_t addr_count             = (address == nullptr) ? 0 : 1;
        callback(reinterpret_cast<void *>(contextHandle), service, Span<Inet::IPAddress>(address, addr_count), error);
    };

    VerifyOrReturn(address != nullptr && port != 0, dispatch(CHIP_ERROR_UNKNOWN_RESOURCE_ID));

    JNIEnv * env = JniReferences::GetInstance().GetEnvForCurrentThread();
    JniUtfString jniInstanceName(env, instanceName);
    JniUtfString jniServiceType(env, serviceType);
    JniUtfString jnihostName(env, hostName);
    JniUtfString jniAddress(env, address);
    Inet::IPAddress ipAddress;
    Inet::InterfaceId iface;

    VerifyOrReturn(strlen(jniInstanceName.c_str()) <= Operational::kInstanceNameMaxLength, dispatch(CHIP_ERROR_INVALID_ARGUMENT));
    VerifyOrReturn(strlen(jniServiceType.c_str()) <= kDnssdTypeAndProtocolMaxSize, dispatch(CHIP_ERROR_INVALID_ARGUMENT));
    VerifyOrReturn(CanCastTo<uint16_t>(port), dispatch(CHIP_ERROR_INVALID_ARGUMENT));
    VerifyOrReturn(Inet::IPAddress::FromString(const_cast<char *>(jniAddress.c_str()), ipAddress, iface),
                   dispatch(CHIP_ERROR_INVALID_ARGUMENT));

    DnssdService service = {};
    CopyString(service.mName, jniInstanceName.c_str());
    CopyString(service.mHostName, jnihostName.c_str());

    VerifyOrReturn(extractProtocol(jniServiceType.c_str(), service.mType, service.mProtocol) == CHIP_NO_ERROR,
                   dispatch(CHIP_ERROR_INVALID_ARGUMENT));

    service.mPort          = static_cast<uint16_t>(port);
    service.mInterface     = iface;
    service.mTextEntrySize = 0;
    service.mTextEntries   = nullptr;

    // Note on alloc/free memory use
    // We are only allocating the entries list and the data field of each entry
    // so we free these in the exit section
    if (textEntries != nullptr)
    {
        jobjectArray keys =
            (jobjectArray) env->CallObjectMethod(sMdnsCallbackObject.ObjectRef(), sGetTextEntryKeysMethod, textEntries);
        auto size           = env->GetArrayLength(keys);
        TextEntry * entries = new (std::nothrow) TextEntry[size];
        VerifyOrExit(entries != nullptr, ChipLogError(Discovery, "entries alloc failure"));
        memset(entries, 0, sizeof(entries[0]) * size);

        service.mTextEntries = entries;
        for (decltype(size) i = 0; i < size; i++)
        {
            jstring jniKeyObject = (jstring) env->GetObjectArrayElement(keys, i);
            JniUtfString key(env, jniKeyObject);
            entries[i].mKey = strdup(key.c_str());

            jbyteArray datas = (jbyteArray) env->CallObjectMethod(sMdnsCallbackObject.ObjectRef(), sGetTextEntryDataMethod,
                                                                  textEntries, jniKeyObject);
            if (datas != nullptr)
            {
                size_t dataSize = env->GetArrayLength(datas);
                uint8_t * data  = new (std::nothrow) uint8_t[dataSize];
                VerifyOrExit(data != nullptr, ChipLogError(Discovery, "data alloc failure"));

                jbyte * jnidata = env->GetByteArrayElements(datas, nullptr);
                for (size_t j = 0; j < dataSize; j++)
                {
                    data[j] = static_cast<uint8_t>(jnidata[j]);
                }
                entries[i].mDataSize = dataSize;
                entries[i].mData     = data;

                ChipLogProgress(Discovery, " ----- entry [%u] : %s %s\n", static_cast<unsigned int>(i), entries[i].mKey,
                                std::string(reinterpret_cast<char *>(data), dataSize).c_str());
            }
            else
            {
                ChipLogProgress(Discovery, " ----- entry [%u] : %s NULL\n", static_cast<unsigned int>(i), entries[i].mKey);

                entries[i].mDataSize = 0;
                entries[i].mData     = nullptr;
            }
            service.mTextEntrySize = size;
        }
    }

exit:
    dispatch(CHIP_NO_ERROR, &service, &ipAddress);

    if (service.mTextEntries != nullptr)
    {
        size_t size = service.mTextEntrySize;
        for (size_t i = 0; i < size; i++)
        {
            delete[] service.mTextEntries[i].mKey;
            if (service.mTextEntries[i].mData != nullptr)
            {
                delete[] service.mTextEntries[i].mData;
            }
        }
        delete[] service.mTextEntries;
    }
}

void HandleBrowse(jobjectArray instanceName, jstring serviceType, jlong callbackHandle, jlong contextHandle)
{
    VerifyOrReturn(callbackHandle != 0, ChipLogError(Discovery, "HandleDiscover called with callback equal to nullptr"));

    const auto dispatch = [callbackHandle, contextHandle](CHIP_ERROR error, DnssdService * service = nullptr, size_t size = 0) {
        DeviceLayer::StackLock lock;
        DnssdBrowseCallback callback = reinterpret_cast<DnssdBrowseCallback>(callbackHandle);
        callback(reinterpret_cast<void *>(contextHandle), service, size, true, error);
    };

    JNIEnv * env = JniReferences::GetInstance().GetEnvForCurrentThread();
    JniUtfString jniServiceType(env, serviceType);

    auto size              = env->GetArrayLength(instanceName);
    DnssdService * service = new DnssdService[size];
    for (decltype(size) i = 0; i < size; i++)
    {
        JniUtfString jniInstanceName(env, (jstring) env->GetObjectArrayElement(instanceName, i));
        VerifyOrReturn(strlen(jniInstanceName.c_str()) <= Operational::kInstanceNameMaxLength,
                       dispatch(CHIP_ERROR_INVALID_ARGUMENT));

        CopyString(service[i].mName, jniInstanceName.c_str());
        VerifyOrReturn(extractProtocol(jniServiceType.c_str(), service[i].mType, service[i].mProtocol) == CHIP_NO_ERROR,
                       dispatch(CHIP_ERROR_INVALID_ARGUMENT));
    }

    dispatch(CHIP_NO_ERROR, service, size);
    delete[] service;
}

} // namespace Dnssd
} // namespace chip
