#pragma once
#include "stream-bridge-sender.h"
#include <nvEncodeAPI.h> // Explicit, hash-pinned SDK 11.1 input; never downloaded at build time.
#include <array>
#include <atomic>
#include <mutex>

namespace stream_bridge::sender {
using NvencFactory = NVENCSTATUS (NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
// Public NVENC ABI boundary. No private Steam object layout or function jump.
// Single process-lifetime instance; factory tables may retain these callbacks.
class NvencBoundary {
public:
    explicit NvencBoundary(const std::array<std::uint8_t,32>& verifiedModuleHash);
    NvencBoundary(const NvencBoundary&)=delete;
    NvencBoundary& operator=(const NvencBoundary&)=delete;
    NVENCSTATUS create(NvencFactory,NV_ENCODE_API_FUNCTION_LIST*);
    // Future AUTHENTICATED control binding only. The shipping prototype never
    // calls this method, so installing this provider alone changes no AU bytes.
    bool requestDiscovery(void* encoder,const LocalConfig&,const VerifiedChannel&,PayloadSealer sealer={});
    Status receiveControl(void* encoder,Bytes,const VerifiedChannel&);
    void stop();
    bool wrapped()const;
    std::uint64_t generation(void* encoder)const;
    std::uint32_t liveLeases()const;
private:
    struct Session {
        void* encoder=nullptr;std::uint64_t generation=0;
        bool initialized=false,hevc=false;
        SenderAdapter adapter{};VerifiedChannel channel{};
        void* output=nullptr;std::uint64_t ticket=0;
    };
    Session* find(void*);
    VerifiedSource source(const Session&)const;
    static NVENCSTATUS NVENCAPI open(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS*,void**);
    static NVENCSTATUS NVENCAPI init(void*,NV_ENC_INITIALIZE_PARAMS*);
    static NVENCSTATUS NVENCAPI reconfigure(void*,NV_ENC_RECONFIGURE_PARAMS*);
    static NVENCSTATUS NVENCAPI lock(void*,NV_ENC_LOCK_BITSTREAM*);
    static NVENCSTATUS NVENCAPI unlock(void*,NV_ENC_OUTPUT_PTR);
    static NVENCSTATUS NVENCAPI destroy(void*);
    static std::atomic<NvencBoundary*> instance_;
    mutable std::mutex mutex_;
    std::array<Session,4> sessions_{};
    std::array<std::uint8_t,32> hash_{};
    NV_ENCODE_API_FUNCTION_LIST original_{};
    std::uint64_t nextGeneration_=0;
    bool installed_=false,enabled_=true;
};
} // namespace stream_bridge::sender
