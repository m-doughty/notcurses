#include "lib/internal.h"
#include "lib/sixel.h"
#include <windows.h>
ncloglevel_e loglevel = NCLOGLEVEL_SILENT;
static char cell(unsigned y, unsigned x, unsigned rows, unsigned cols){
  if(y == 0 || y == rows - 1) return x == 0 || x == cols - 1 ? '+' : '-';
  if(x == 0 || x == cols - 1 || x == cols * 3 / 4) return '|';
  if(y >= 3 && y < 10 && x >= 4 && x < 24) return ' ';
  if(y % 4 == 1 && x % 16 < 10) return "0123456789"[(y + x) % 10];
  return ' ';
}
int main(int argc, char** argv){
  if(argc != 2) return 2;
  char logpath[4096]; snprintf(logpath,sizeof(logpath),"%s/physical-images.log",argv[1]); FILE* log=fopen(logpath,"w");
  if(!log) return 2;
  notcurses_options opts={0}; opts.flags=NCOPTION_SUPPRESS_BANNERS;
  notcurses* nc=notcurses_core_init(&opts,stdout); if(!nc) return 3;
  if(notcurses_check_pixel_support(nc)!=NCPIXEL_SIXEL) return 4;
  unsigned pr=0,pc=0,py=0,px=0, frames=0;
  ncplane* bitmap=NULL;
  uint64_t checked=0;
  for(unsigned i=0;i<200;++i){
    unsigned r,c,y,x;
    if(notcurses_poll_geometry(nc,&r,&c,&y,&x)||!y||!x) return 5;
    const bool changed=r!=pr||c!=pc||y!=py||x!=px;
    if(changed||i%3==0){
      if(bitmap){ncplane_destroy(bitmap);bitmap=NULL;}
      for(unsigned a=0;a<r;++a) for(unsigned b=0;b<c;++b){
        char s[2]={cell(a,b,r,c),0};ncplane_putstr_yx(notcurses_stdplane(nc),a,b,s);
      }
      unsigned height=6*y/6*6, width=18*x;
      unsigned begy=(i/3)%(2*y), begx=3, stride=width+begx;
      unsigned sourceh=height+2*y;
      uint32_t* rgba=malloc((size_t)sourceh*stride*4);if(!rgba)return 6;
      const uint32_t colors[]={0xff0000ff,0xff00ff00,0xffff0000,0xff00ffff};
      for(unsigned a=0;a<sourceh;++a)for(unsigned b=0;b<stride;++b)
        rgba[a*stride+b]=b==stride-1?0xffff00ff:colors[(a/7)%4];
      struct ncvisual* v=ncvisual_from_rgba(rgba,sourceh,stride*4,stride);if(!v)return 7;
      ncplane_options po={0};po.rows=6;po.cols=18;po.y=3;po.x=4;
      bitmap=ncplane_create(notcurses_stdplane(nc),&po);if(!bitmap)return 8;
      struct ncvisual_options vo={0};vo.n=bitmap;
      vo.blitter=NCBLIT_PIXEL;vo.scaling=NCSCALE_NONE;
      vo.flags=NCVISUAL_OPTION_NODEGRADE|NCVISUAL_OPTION_NOINTERPOLATE;
      vo.begy=begy;vo.begx=begx;vo.leny=height;vo.lenx=width;
      bitmap=ncvisual_blit(nc,v,&vo);if(!bitmap)return 8;
      FILE* capture=tmpfile();if(!capture)return 9;
      nc->ttyfp=capture;int rc=notcurses_render(nc);nc->ttyfp=stdout;if(rc)return 10;
      fflush(capture);rewind(capture);char chunk[8192];size_t len;
      while((len=fread(chunk,1,sizeof(chunk),capture)))fwrite(chunk,1,len,stdout);
      fflush(stdout);fclose(capture);
      sprixel* s=bitmap->sprite;if(!s)return 11;
      // Decode the exact payload emitted by this render, including cropping
      // and any wipe/rebuild, rather than trusting the pre-encoded RGBA.
      uint32_t* decoded=ncsixel_as_rgba(s->glyph.buf,s->pixy,s->pixx);if(!decoded)return 12;
      if(s->pixy!=(int)height||s->pixx!=(int)width){fprintf(log,"geometry i=%u cell=%ux%u requested=%ux%u actual=%dx%d\n",i,y,x,height,width,s->pixy,s->pixx);fclose(log);return 13;}
      for(unsigned a=0;a<height;++a)for(unsigned b=0;b<width;++b){
        // This encoder caps palette components at 99%, decoded as 252.
        uint32_t expected=rgba[(begy+a)*stride+begx+b]&0xfffcfcfc;
        if(decoded[a*width+b]!=expected){
          fprintf(log,"FAIL i=%u cell=%ux%u pixel=%u,%u expected=%08x actual=%08x\n",i,y,x,a,b,rgba[(begy+a)*stride+begx+b],decoded[a*width+b]);fflush(log);return 14;
        }
        ++checked;
      }
      free(decoded);free(rgba);ncvisual_destroy(v);++frames;
      if(changed){fprintf(log,"i=%u grid=%ux%u cell=%ux%u pixels=%ux%u\n",i,r,c,y,x,height,width);fflush(log);}
      pr=r;pc=c;py=y;px=x;
    }
    if(i==40||i==80||i==120){printf("\x1b]1337;SetUserVar=fontzoom=%s\a",i==40?"Mg==":i==80?"Mw==":"NA==");fflush(stdout);}
    if(i==20||i==70||i==110||i==160){
      unsigned id=i==20?1:i==70?2:i==110?3:4;char path[4096];
      snprintf(path,sizeof(path),"%s/expected-%u.txt",argv[1],id);
      FILE* f=fopen(path,"w");if(!f)return 15;
      for(unsigned a=0;a<r;++a){for(unsigned b=0;b<c;++b)fputc(cell(a,b,r,c),f);fputc('\n',f);}fclose(f);
      const char* ids[]={"","MQ==","Mg==","Mw==","NA=="};
      printf("\x1b]1337;SetUserVar=physicalcapture=%s\a",ids[id]);fflush(stdout);
    }
    Sleep(100);
  }
  fprintf(log,"PASS frames=%u decoded-pixels=%llu\n",frames,(unsigned long long)checked);fclose(log);
  return notcurses_stop(nc);
}
