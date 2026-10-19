// Exercise the actual process cold readers, not the renderer fixture's supplied
// in-memory artifact observation. No Steam executable, thread or device starts.
#define main inherited_process_lifecycle_tests_not_run
#include "lifecycle_tests.cpp"
#undef main
#include <fstream>
#include <fcntl.h>

namespace {
std::vector<std::uint8_t> bytes(const char* name){
    std::ifstream file(name,std::ios::binary|std::ios::ate);
    check(bool(file),"private generated shader input opens");
    const auto count=file.tellg();check(count>0&&count<65536,"bounded private shader input");
    std::vector<std::uint8_t> result(static_cast<std::size_t>(count));file.seekg(0);
    file.read(reinterpret_cast<char*>(result.data()),count);check(bool(file),"complete private shader input");return result;
}
void write(const std::filesystem::path& path,const std::vector<std::uint8_t>& value){
    std::ofstream out(path,std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char*>(value.data()),value.size());out.close();
    check(bool(out)&&chmod(path.c_str(),0600)==0,"owned private shader file written");
}
void artifactReaders(const std::vector<std::uint8_t>& coverage,const std::vector<std::uint8_t>& retired,const std::vector<std::uint8_t>& array3){
    Directory directory;auto* p=renderer::process();
    check(p&&p->directory<0&&!renderer::enabled(),"fixture never activates a native process or worker");
    p->directory=open(directory.path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);check(p->directory>=0,"actual private artifact directory retained");
    struct Close {renderer::Process* p;~Close(){close(p->directory);p->directory=-1;}} close{p};
    const auto coveragePath=directory.path/"coverage.spv",legacyPath=directory.path/"legacy.spv";
    write(coveragePath,coverage);write(legacyPath,retired);
    std::vector<std::uint32_t> out;
    check(renderer::readCoverageFragmentCold(out),"actual installed count1 coverage accepted by production cold loader");
    check(out.size()*4==coverage.size()&&!std::memcmp(out.data(),coverage.data(),coverage.size()),"cold coverage bytes exactly equal installed artifact");
    const std::vector<std::uint32_t> sentinel{0x12345678};
    const auto refuses=[&](const char* message){out=sentinel;check(!renderer::readCoverageFragmentCold(out)&&out==sentinel,message);};
    std::filesystem::remove(coveragePath);refuses("missing coverage refuses without publishing partial output");
    refuses("obsolete atlas file cannot replace missing exact coverage");
    auto changed=coverage;changed.back()^=1;write(coveragePath,changed);refuses("changed exact-size coverage refuses");
    write(coveragePath,array3);refuses("historical count3 coverage cannot satisfy the installed scalar pin");
    write(coveragePath,coverage);check(chmod(coveragePath.c_str(),0644)==0,"invalid permission fixture set");refuses("publicly readable coverage refuses");
    check(chmod(coveragePath.c_str(),0600)==0,"private permissions restored");std::filesystem::create_hard_link(coveragePath,directory.path/"alias");refuses("multiply linked coverage refuses");std::filesystem::remove(directory.path/"alias");
    std::filesystem::rename(coveragePath,directory.path/"target");std::filesystem::create_symlink("target",coveragePath);refuses("symlink coverage refuses");std::filesystem::remove(coveragePath);std::filesystem::rename(directory.path/"target",coveragePath);
    check(renderer::readCoverageFragmentCold(out),"restoring exact owned count1 file restores cold admission");
    std::filesystem::remove(legacyPath);
    check(renderer::readCoverageFragmentCold(out),"removing obsolete atlas file cannot affect active exact coverage");
}
}
int main(int argc,char** argv){try{check(argc==4,"explicit coverage, retired atlas and historical count3 private inputs required");artifactReaders(bytes(argv[1]),bytes(argv[2]),bytes(argv[3]));std::printf("%u production artifact cold-reader checks passed\n",checks);return 0;}catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}}
