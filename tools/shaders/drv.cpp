#include "shader.h"
#include "shader_recompiler.h"
#include <cstdio>
#include <sys/wait.h>
#include <unistd.h>
static std::vector<uint8_t> readAll(const std::filesystem::path& p){FILE*f=fopen(p.c_str(),"rb");std::vector<uint8_t> d;if(!f)return d;fseek(f,0,SEEK_END);d.resize(ftell(f));fseek(f,0,SEEK_SET);fread(d.data(),1,d.size(),f);fclose(f);return d;}
int main(int argc,char**argv){
  auto inc=readAll(argv[3]); std::string_view include((const char*)inc.data(),inc.size());
  int ok=0,bad=0,crash=0;
  for(auto& e: std::filesystem::recursive_directory_iterator(argv[1])){
    if(e.is_directory())continue;
    auto ext=e.path().extension().string(); if(ext!=".fpo"&&ext!=".vpo")continue;
    auto d=readAll(e.path());
    auto* c=reinterpret_cast<const ShaderContainer*>(d.data());
    if(d.size()<sizeof(ShaderContainer)||(c->flags&0xFFFFFF00)!=0x102A1100){++bad;printf("BADHDR %s\n",e.path().c_str());continue;}
    pid_t pid=fork();
    if(pid==0){
      ShaderRecompiler r; r.recompile(d.data(),include);
      auto out=std::filesystem::path(argv[2])/std::filesystem::relative(e.path(),argv[1]);
      std::filesystem::create_directories(out.parent_path());
      out+=".hlsl"; FILE*f=fopen(out.c_str(),"wb"); fwrite(r.out.data(),1,r.out.size(),f); fclose(f);
      _exit(0);
    }
    int st=0; waitpid(pid,&st,0);
    if(WIFEXITED(st)&&WEXITSTATUS(st)==0) ++ok; else {++crash; printf("CRASH %s\n",e.path().c_str());}
  }
  printf("ok %d crash %d badheader %d\n",ok,crash,bad);
}
