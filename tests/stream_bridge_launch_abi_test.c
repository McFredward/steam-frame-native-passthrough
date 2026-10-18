#define _GNU_SOURCE 1
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
static unsigned checks;
#define CHECK(v) do{++checks;if(!(v)){fprintf(stderr,"ABI line %d\n",__LINE__);return 1;}}while(0)
int main(void){
    unsigned(*mask)(void)=NULL;void* address=dlsym(RTLD_DEFAULT,"frame_compositor_launch_provider_mask");
    const int baseline=getenv("SFNP_LAUNCH_ABI_BASELINE")!=NULL;
    memcpy(&mask,&address,sizeof mask);CHECK(baseline?!mask:(mask&&mask()==63));
    char* args[]={"/sfnp-no-such-program",NULL};errno=77;
    CHECK(execve(args[0],args,environ)==-1&&errno==ENOENT);
    pid_t pid=-1;errno=77;const int result=posix_spawn(&pid,args[0],NULL,NULL,args,environ);const int saved=errno;
    printf("native spawn result=%d errno=%d\n",result,saved);
    if(result==0){int child_status=0;CHECK(waitpid(pid,&child_status,0)==pid);printf("native spawn child=%d\n",child_status);}
    pid=fork();CHECK(pid>=0);if(pid==0)_exit((baseline||mask()==63)?0:2);
    int status=0;CHECK(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    printf("AArch64 public launch ABI: %u checks PASS\n",checks);return 0;
}
