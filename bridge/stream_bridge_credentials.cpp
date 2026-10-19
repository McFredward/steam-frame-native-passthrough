#include "stream_bridge_credentials.h"
#include <algorithm>
#include <cstring>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <bcrypt.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace stream_bridge {
namespace {
struct FileIdentity{std::array<std::uint64_t,8> fields{};bool operator==(const FileIdentity& other)const{return fields==other.fields;}};
struct Buffer{std::array<char,4097> data{};std::size_t size=0;~Buffer(){secureZero(data.data(),data.size());}};
struct Span{const char* data=nullptr;std::size_t size=0;bool string=false;};
bool equal(Span s,const char* text){const auto n=std::strlen(text);return s.size==n&&std::memcmp(s.data,text,n)==0;}
bool white(char c){return c==' '||c=='\r'||c=='\n'||c=='\t';}
class Object{
    const Buffer& b_;std::size_t at_=0;
    void ws(){while(at_<b_.size&&white(b_.data[at_]))++at_;}
    bool symbol(char c){ws();if(at_==b_.size||b_.data[at_]!=c)return false;++at_;return true;}
    bool token(Span& out){ws();if(at_==b_.size)return false;const auto start=at_;const bool quoted=b_.data[at_]=='"';
        if(quoted){++at_;const auto begin=at_;while(at_<b_.size&&b_.data[at_]!='"'){const unsigned char c=b_.data[at_];if(c<0x20||c>0x7e||c=='\\')return false;++at_;}if(at_==b_.size)return false;out={b_.data.data()+begin,at_-begin,true};++at_;return true;}
        while(at_<b_.size&&b_.data[at_]>='0'&&b_.data[at_]<='9')++at_;
        if(at_==start||(at_-start>1&&b_.data[start]=='0'))return false;
        out={b_.data.data()+start,at_-start,false};return true;
    }
public:
    explicit Object(const Buffer& b):b_(b){}
    bool parse(std::array<Span,5>& fields,unsigned& seen){
        if(!symbol('{'))return false;
        seen=0;ws();if(at_<b_.size&&b_.data[at_]=='}')return false;
        for(unsigned entries=0;entries<5;++entries){Span key,value;if(!token(key)||!key.string||!symbol(':')||!token(value))return false;
            unsigned index=5;const char* names[]={"schema","pair_id","key_hex","role","protocol_major"};
            for(unsigned i=0;i<5;++i)if(equal(key,names[i])){index=i;break;}
            if(index==5||(seen&(1u<<index)))return false;
            fields[index]=value;seen|=1u<<index;ws();
            if(at_<b_.size&&b_.data[at_]=='}'){++at_;ws();return at_==b_.size;}
            if(!symbol(','))return false;
        }return false;
    }
};
bool decodeHex(Span span,std::uint8_t* out,std::size_t bytes){
    if(!span.string||span.size!=bytes*2)return false;
    for(std::size_t i=0;i<bytes;++i){unsigned v=0;for(unsigned j=0;j<2;++j){char c=span.data[2*i+j];unsigned x;if(c>='0'&&c<='9')x=unsigned(c-'0');else if(c>='a'&&c<='f')x=unsigned(c-'a'+10);else return false;v=(v<<4)|x;}out[i]=static_cast<std::uint8_t>(v);}return true;
}
#ifdef _WIN32
bool privateHandle(HANDLE file){
    PSID owner=nullptr;PACL acl=nullptr;PSECURITY_DESCRIPTOR sd=nullptr;
    if(GetSecurityInfo(file,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,nullptr,&acl,nullptr,&sd)!=ERROR_SUCCESS)return false;
    HANDLE token=nullptr;bool okay=false;
    if(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)){
        DWORD bytes=0;GetTokenInformation(token,TokenUser,nullptr,0,&bytes);
        std::array<std::uint8_t,1024> user{};
        if(bytes&&bytes<=user.size()&&GetTokenInformation(token,TokenUser,user.data(),bytes,&bytes)&&EqualSid(owner,reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid)){
            LPWSTR text=nullptr;
            if(ConvertSecurityDescriptorToStringSecurityDescriptorW(sd,SDDL_REVISION_1,DACL_SECURITY_INFORMATION,&text,nullptr)){
                okay=std::wcscmp(text,L"D:P(A;;FA;;;SY)(A;;FA;;;OW)")==0;LocalFree(text);
            }
        }CloseHandle(token);
    }LocalFree(sd);return okay;
}
struct Directory{
    std::array<HANDLE,64> handles{};std::size_t used=0;
    ~Directory(){for(std::size_t i=0;i<used;++i)CloseHandle(handles[i]);}
    bool open(const std::filesystem::path& requested){
        const auto absolute=std::filesystem::absolute(requested);auto current=absolute.root_path();
        if(current.empty())return false;
        for(const auto& part:absolute.relative_path()){
            if(part==L"."||part.empty())continue;
            if(part==L".."||used==handles.size())return false;
            current/=part;
            HANDLE h=CreateFileW(current.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            if(h==INVALID_HANDLE_VALUE)return false;
            BY_HANDLE_FILE_INFORMATION info{};
            if(!GetFileInformationByHandle(h,&info)||!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){CloseHandle(h);return false;}
            handles[used++]=h;
        }return used!=0;
    }
};
FileIdentity identity(const BY_HANDLE_FILE_INFORMATION& x){return {{{x.dwVolumeSerialNumber,x.nFileIndexHigh,x.nFileIndexLow,x.nFileSizeHigh,x.nFileSizeLow,(std::uint64_t(x.ftCreationTime.dwHighDateTime)<<32)|x.ftCreationTime.dwLowDateTime,(std::uint64_t(x.ftLastWriteTime.dwHighDateTime)<<32)|x.ftLastWriteTime.dwLowDateTime,x.dwFileAttributes}}};}
CredentialStatus read(const std::filesystem::path& directory,const std::filesystem::path& name,Buffer& output,CredentialStatus missing,FileIdentity* pinned=nullptr){
    const auto full=directory/name;
    HANDLE file=CreateFileW(full.c_str(),GENERIC_READ|READ_CONTROL,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file==INVALID_HANDLE_VALUE)return GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND?missing:CredentialStatus::IoError;
    BY_HANDLE_FILE_INFORMATION info{};CredentialStatus result=CredentialStatus::InsecureFile;
    if(GetFileInformationByHandle(file,&info)&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&info.nNumberOfLinks==1&&privateHandle(file)){
        if(info.nFileSizeHigh||info.nFileSizeLow>4096)result=CredentialStatus::IoError;
        else{DWORD got=0;BY_HANDLE_FILE_INFORMATION after{};const bool okay=ReadFile(file,output.data.data(),info.nFileSizeLow,&got,nullptr)&&got==info.nFileSizeLow&&GetFileInformationByHandle(file,&after)&&identity(info)==identity(after);result=okay?CredentialStatus::Ok:CredentialStatus::IoError;output.size=got;if(okay&&pinned)*pinned=identity(info);}
    }CloseHandle(file);return result;
}
#else
struct Directory{int fd=-1;~Directory(){if(fd>=0)::close(fd);}
    bool open(const std::filesystem::path& requested){
        const auto absolute=std::filesystem::absolute(requested);fd=::open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(fd<0)return false;
        unsigned depth=0;
        for(const auto& part:absolute.relative_path()){
            if(part=="."||part.empty())continue;
            if(part==".."||++depth>64){errno=ELOOP;return false;}
            const int next=openat(fd,part.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(next<0)return false;
            ::close(fd);fd=next;
        }return true;
    }
};
FileIdentity identity(const struct stat& x){return {{{std::uint64_t(x.st_dev),std::uint64_t(x.st_ino),std::uint64_t(x.st_size),std::uint64_t(x.st_mtim.tv_sec),std::uint64_t(x.st_mtim.tv_nsec),std::uint64_t(x.st_ctim.tv_sec),std::uint64_t(x.st_ctim.tv_nsec),std::uint64_t(x.st_mode)}}};}
CredentialStatus read(int directory,const char* name,Buffer& output,CredentialStatus missing,FileIdentity* pinned=nullptr){
    int file=openat(directory,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(file<0)return errno==ENOENT?missing:errno==ELOOP?CredentialStatus::InsecureFile:CredentialStatus::IoError;
    struct stat before{},after{};CredentialStatus result=CredentialStatus::InsecureFile;
    if(fstat(file,&before)==0&&S_ISREG(before.st_mode)&&before.st_uid==geteuid()&&before.st_nlink==1&&(before.st_mode&0777)==0600){
        if(before.st_size<0||before.st_size>4096)result=CredentialStatus::IoError;
        else{std::size_t used=0;bool okay=true;
            while(used<static_cast<std::size_t>(before.st_size)){const auto n=::read(file,output.data.data()+used,static_cast<std::size_t>(before.st_size)-used);if(n<0&&errno==EINTR)continue;if(n<=0){okay=false;break;}used+=static_cast<std::size_t>(n);}
            okay=okay&&fstat(file,&after)==0&&before.st_dev==after.st_dev&&before.st_ino==after.st_ino&&before.st_size==after.st_size&&before.st_mtim.tv_sec==after.st_mtim.tv_sec&&before.st_mtim.tv_nsec==after.st_mtim.tv_nsec&&before.st_ctim.tv_sec==after.st_ctim.tv_sec&&before.st_ctim.tv_nsec==after.st_ctim.tv_nsec;
            result=okay?CredentialStatus::Ok:CredentialStatus::IoError;output.size=used;if(okay&&pinned)*pinned=identity(before);
        }
    }::close(file);return result;
}
#endif
}
CredentialStatus loadPrivatePairing(const std::filesystem::path& directory,Role role,Pairing& output) noexcept{
    try{
        Buffer active,credential,selectionAgain;FileIdentity selected{},selectedAgain{};std::array<Span,5> fields{};unsigned seen=0;
#ifndef _WIN32
        Directory dir;if(!dir.open(directory))return errno==ENOENT?CredentialStatus::MissingSelection:errno==ELOOP||errno==ENOTDIR?CredentialStatus::InsecureFile:CredentialStatus::IoError;
        struct stat d{};if(fstat(dir.fd,&d)!=0||d.st_uid!=geteuid()||(d.st_mode&077)!=0)return CredentialStatus::InsecureFile;
        auto result=read(dir.fd,"active.json",active,CredentialStatus::MissingSelection,&selected);
#else
        Directory dir;if(!dir.open(directory))return GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND?CredentialStatus::MissingSelection:CredentialStatus::InsecureFile;
        auto result=read(directory,L"active.json",active,CredentialStatus::MissingSelection,&selected);
#endif
        if(result!=CredentialStatus::Ok)return result;
        Pairing candidate;struct Erase{Pairing& p;~Erase(){secureZero(p.key.data(),p.key.size());}}erase{candidate};
        if(!Object(active).parse(fields,seen)||seen!=3||!fields[0].string||!equal(fields[0],"sfnp-active-pairing-v1")||!decodeHex(fields[1],candidate.pairId.data(),candidate.pairId.size()))return CredentialStatus::MalformedSelection;
        std::array<char,38> name{};std::copy_n(fields[1].data,32,name.data());std::memcpy(name.data()+32,".json",5);
#ifndef _WIN32
        result=read(dir.fd,name.data(),credential,CredentialStatus::MissingCredential);
#else
        result=read(directory,std::filesystem::path(name.data()),credential,CredentialStatus::MissingCredential);
#endif
        if(result!=CredentialStatus::Ok)return result;
        fields={};seen=0;
        if(!Object(credential).parse(fields,seen)||seen!=31||!fields[0].string||!equal(fields[0],"sfnp-pairing-v1")
            ||!fields[1].string||fields[1].size!=32||std::memcmp(fields[1].data,name.data(),32)!=0||!decodeHex(fields[2],candidate.key.data(),candidate.key.size())||!fields[3].string)return CredentialStatus::MalformedCredential;
        if(fields[4].string||!equal(fields[4],"1"))return CredentialStatus::UnsupportedProtocol;
        if(role!=Role::Producer&&role!=Role::Receiver)return CredentialStatus::RoleMismatch;
        if(!equal(fields[3],role==Role::Producer?"producer":"receiver"))return CredentialStatus::RoleMismatch;
        if(std::all_of(candidate.pairId.begin(),candidate.pairId.end(),[](auto b){return b==0;})||std::all_of(candidate.key.begin(),candidate.key.end(),[](auto b){return b==0;}))return CredentialStatus::MalformedCredential;
        // Recheck the selected file itself, not only its public pair ID, before publishing a key.
#ifndef _WIN32
        result=read(dir.fd,"active.json",selectionAgain,CredentialStatus::MissingSelection,&selectedAgain);
#else
        result=read(directory,L"active.json",selectionAgain,CredentialStatus::MissingSelection,&selectedAgain);
#endif
        if(result!=CredentialStatus::Ok)return result;
        if(!(selected==selectedAgain)||active.size!=selectionAgain.size||std::memcmp(active.data.data(),selectionAgain.data.data(),active.size)!=0)return CredentialStatus::IoError;
        candidate.role=role;output=candidate;return CredentialStatus::Ok;
    }catch(...){return CredentialStatus::IoError;}
}
const char* credentialStatusName(CredentialStatus status) noexcept{
    switch(status){case CredentialStatus::Ok:return "ready";case CredentialStatus::MissingSelection:return "pairing-not-selected";
    case CredentialStatus::MissingCredential:return "selected-pairing-missing";case CredentialStatus::InsecureFile:return "pairing-permissions-refused";
    case CredentialStatus::MalformedSelection:return "pairing-selection-invalid";case CredentialStatus::MalformedCredential:return "pairing-file-invalid";
    case CredentialStatus::RoleMismatch:return "pairing-role-mismatch";case CredentialStatus::UnsupportedProtocol:return "pairing-version-mismatch";
    default:return "pairing-read-failed";}
}
bool bridgeSystemRandom(void* output,std::size_t size) noexcept{
    if(!output||!size||size>64)return false;
#ifdef _WIN32
    return BCryptGenRandom(nullptr,static_cast<PUCHAR>(output),static_cast<ULONG>(size),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0;
#else
    auto* data=static_cast<std::uint8_t*>(output);std::size_t used=0;
    while(used<size){const auto n=getrandom(data+used,size-used,GRND_NONBLOCK);if(n<0&&errno==EINTR)continue;if(n<=0){secureZero(output,size);return false;}used+=static_cast<std::size_t>(n);}return true;
#endif
}
} // namespace stream_bridge
