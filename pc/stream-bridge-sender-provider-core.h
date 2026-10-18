#pragma once
#include "stream-bridge-sender.h"
#include <nvEncodeAPI.h> // Explicit, hash-pinned SDK 11.1 input; never downloaded at build time.
#include <array>
#include <atomic>
#include <mutex>

namespace stream_bridge::sender {
using NvencFactory = NVENCSTATUS (NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
struct NvencOutputEvidence {
    void* encoder=nullptr;std::uint64_t encoderGeneration=0,submission=0;
    void* resource=nullptr;std::uint64_t resourceGeneration=0,mapGeneration=0;
    NV_ENC_INPUT_PTR mappedInput=nullptr;NV_ENC_OUTPUT_PTR output=nullptr;
    std::uint64_t inputTimestamp=0;std::uint32_t width=0,height=0,subresource=0;
    NV_ENC_BUFFER_FORMAT format=NV_ENC_BUFFER_FORMAT_UNDEFINED;
    bool exactInput=false;
};
struct DeviceObserver {
    // Successful actual public Open session. Device is borrowed native identity;
    // a D3D11 consumer must QueryInterface and retain its own COM references.
    void (*opened)(void* encoder,std::uint64_t generation,void* device,NV_ENC_DEVICE_TYPE,void*)=nullptr;
    void* context=nullptr;
};
struct InputObserver {
    // Bounded ledger capture before original Encode; acceptance is NOT asserted
    // until its successful result and exact matching Lock. No file/network/GPU
    // waits or reentry to this boundary are permitted in these callbacks.
    void (*capture)(const NvencOutputEvidence&,void*)=nullptr;
    void (*retire)(const NvencOutputEvidence&,void*)=nullptr;
    void* context=nullptr;
};
struct OutputAugmenter {
    // Original/native pixels are untouched. The candidate can already contain
    // discovery SEI. Rejecting the final augmentation restores ORIGINAL AU and
    // completes any preliminary discovery ticket as unsent.
    bool (*augment)(Bytes,const NvencOutputEvidence&,std::vector<std::uint8_t>&,void*)=nullptr;
    void* context=nullptr;
};
struct EncoderState{void* encoder=nullptr;std::uint64_t generation=0;bool hevc=false,peerConfirmed=false,probeObserved=false;std::uint32_t width=0,height=0;bool encoded=false;};
// Public NVENC ABI boundary. No private Steam object layout or function jump.
// Single process-lifetime instance; factory tables may retain these callbacks.
class NvencBoundary {
public:
    explicit NvencBoundary(const std::array<std::uint8_t,32>& verifiedModuleHash);
    NvencBoundary(const NvencBoundary&)=delete;
    NvencBoundary& operator=(const NvencBoundary&)=delete;
    NVENCSTATUS create(NvencFactory,NV_ENCODE_API_FUNCTION_LIST*);
    // Authenticated local authority only. The explicit provider worker starts
    // discovery after private pairing and an actual supported encoder exist.
    bool requestDiscovery(void* encoder,const LocalConfig&,const VerifiedChannel&,PayloadSealer sealer={});
    Status receiveControl(void* encoder,Bytes,const VerifiedChannel&);
    bool requestProbe(void* encoder,std::uint64_t generation,std::uint64_t sequence,const Nonce&);
    Status receiveEcho(void* encoder,Bytes,const VerifiedChannel&);
    void setOutputAugmenter(OutputAugmenter);
    void setInputObserver(InputObserver);
    void setDeviceObserver(DeviceObserver);
    std::array<EncoderState,4> encoderStates()const;
    // Revokes only that observed encoder generation; borrowed output stays alive.
    void revokeDiscovery(void*,std::uint64_t generation);
    void stop();
    bool wrapped()const;
    std::uint64_t generation(void* encoder)const;
    std::uint32_t liveLeases()const;
private:
    struct Session {
        void* encoder=nullptr;std::uint64_t generation=0;
        bool initialized=false,hevc=false,encoded=false;std::uint32_t width=0,height=0;
        SenderAdapter adapter{};VerifiedChannel channel{};
        struct Resource {NV_ENC_REGISTERED_PTR registered=nullptr;void* native=nullptr;std::uint64_t generation=0;std::uint32_t width=0,height=0,subresource=0;NV_ENC_BUFFER_FORMAT format=NV_ENC_BUFFER_FORMAT_UNDEFINED;};
        struct Map {NV_ENC_INPUT_PTR mapped=nullptr;NV_ENC_REGISTERED_PTR registered=nullptr;std::uint64_t generation=0,resourceGeneration=0;};
        struct Submission {NvencOutputEvidence evidence{};bool accepted=false,locked=false;};
        std::array<Resource,32> resources{};std::array<Map,32> maps{};std::array<Submission,32> submissions{};
        std::uint64_t nextIdentity=0,probeSequence=0;Nonce probeToken{};bool probeQueued=false;
        void* output=nullptr;std::uint64_t ticket=0;std::vector<std::uint8_t> augmented{};
    };
    Session* find(void*);
    std::uint64_t revoke(Session&,bool retainRegistrations=false);
    VerifiedSource source(const Session&)const;
    static NVENCSTATUS NVENCAPI open(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS*,void**);
    static NVENCSTATUS NVENCAPI init(void*,NV_ENC_INITIALIZE_PARAMS*);
    static NVENCSTATUS NVENCAPI reconfigure(void*,NV_ENC_RECONFIGURE_PARAMS*);
    static NVENCSTATUS NVENCAPI registerResource(void*,NV_ENC_REGISTER_RESOURCE*);
    static NVENCSTATUS NVENCAPI unregisterResource(void*,NV_ENC_REGISTERED_PTR);
    static NVENCSTATUS NVENCAPI mapInput(void*,NV_ENC_MAP_INPUT_RESOURCE*);
    static NVENCSTATUS NVENCAPI unmapInput(void*,NV_ENC_INPUT_PTR);
    static NVENCSTATUS NVENCAPI encode(void*,NV_ENC_PIC_PARAMS*);
    static NVENCSTATUS NVENCAPI lock(void*,NV_ENC_LOCK_BITSTREAM*);
    static NVENCSTATUS NVENCAPI unlock(void*,NV_ENC_OUTPUT_PTR);
    static NVENCSTATUS NVENCAPI destroy(void*);
    static std::atomic<NvencBoundary*> instance_;
    mutable std::mutex mutex_;
    std::array<Session,4> sessions_{};
    std::array<std::uint8_t,32> hash_{};
    NV_ENCODE_API_FUNCTION_LIST original_{};
    std::uint64_t nextGeneration_=0;
    bool installed_=false,enabled_=true,inputHooks_=false;
    OutputAugmenter augmenter_{};InputObserver inputObserver_{};DeviceObserver deviceObserver_{};
};
} // namespace stream_bridge::sender
