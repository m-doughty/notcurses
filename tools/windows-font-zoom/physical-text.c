#include <notcurses/notcurses.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
static char cell(unsigned y, unsigned x, unsigned rows, unsigned cols){
  if(y == 0 || y == rows - 1) return x == 0 || x == cols - 1 ? '+' : '-';
  if(x == 0 || x == cols - 1 || x == cols * 3 / 4) return '|';
  if(y % 4 == 1 && x % 16 < 10) return "0123456789"[(y + x) % 10];
  return ' ';
}
int main(int argc, char** argv){
  if(argc < 2 || argc > 3) return 2;
  notcurses_options opts = {0}; opts.flags = NCOPTION_SUPPRESS_BANNERS;
  struct notcurses* nc = notcurses_core_init(&opts, stdout);
  if(!nc) return 3;
  char logpath[4096]; snprintf(logpath,sizeof(logpath),"%s/physical-text.log",argv[1]); FILE* log = fopen(logpath, "w");
  if(!log) return 4;
  unsigned pr=0,pc=0,py=0,px=0;
  for(unsigned i=0;i<200;++i){
    unsigned r,c,y,x;
    if(notcurses_poll_geometry(nc,&r,&c,&y,&x)) return 5;
    if(r!=pr || c!=pc || y!=py || x!=px){
      fprintf(log,"i=%u grid=%ux%u cell=%ux%u\n",i,r,c,y,x); fflush(log);
      struct ncplane* n=notcurses_stdplane(nc);
      for(unsigned a=0;a<r;++a) for(unsigned b=0;b<c;++b){
        char s[2]={cell(a,b,r,c),0}; ncplane_putstr_yx(n,a,b,s);
      }
      if(notcurses_render(nc)) return 6;
      if(argc == 3 && atoi(argv[2])) notcurses_refresh(nc,NULL,NULL);
      pr=r;pc=c;py=y;px=x;
    }
    if(i==40 || i==80 || i==120){
      printf("\x1b]1337;SetUserVar=fontzoom=%s\a",i==40?"Mg==":i==80?"Mw==":"NA=="); fflush(stdout);
    }
    if(i==180) notcurses_refresh(nc,NULL,NULL);
    if(i==20 || i==70 || i==110 || i==160 || i==190){
      unsigned id=i==20?1:i==70?2:i==110?3:i==160?4:5;
      char path[4096]; snprintf(path,sizeof(path),"%s/expected-%u.txt",argv[1],id);
      FILE* f=fopen(path,"w"); if(!f) return 7;
      for(unsigned a=0;a<r;++a){for(unsigned b=0;b<c;++b) fputc(cell(a,b,r,c),f);fputc('\n',f);} fclose(f);
      const char* ids[]={"","MQ==","Mg==","Mw==","NA==","NQ=="};
      printf("\x1b]1337;SetUserVar=physicalcapture=%s\a",ids[id]); fflush(stdout);
    }
    Sleep(100);
  }
  fclose(log); return notcurses_stop(nc);
}
