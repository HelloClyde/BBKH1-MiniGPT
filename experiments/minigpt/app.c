#include "h1_sdk.h"
#include "engine.h"
#ifdef MG_INT4
#include "q4_dot.h"
#endif
#ifdef MG_MXU
#include "mxu_dot.h"
#endif
#include "perf.h"
#include "tokenizer.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "font_data.h"
#include "native_input.h"
#include "platform/touch.h"
#include "platform/diagnostics.h"
#ifdef MG_INT4
#define MG_DATA_DIR "A:\\MiniGPT4\\"
#define MG_MODEL_NAME "model.mg4"
#else
#define MG_DATA_DIR "A:\\MiniGPT\\"
#define MG_MODEL_NAME "model.mg8"
#endif
#if defined(MG_DIAGNOSTICS) && defined(MG_DMA_PIPELINE)
#include "dma_hook.h"
extern volatile uint32_t mg_dma_status[8];
#endif

/* Debug counters are also used by the complete-firmware verification harness. */
volatile struct {uint32_t magic,state,input_tokens,prefill,layer,generated,last_token,load_bytes,resident,error,start_tick,end_tick;} minigpt_status;
volatile int minigpt_key_trace[64];
volatile unsigned minigpt_key_events;
#ifdef MG_PREFIX_CACHE
volatile unsigned minigpt_prefill_reused;
#endif
static mg_model model;
#ifdef MG_LAYER_CACHE
static uint32_t reader_offset;static int reader_valid;
#ifdef MG_DMA_PIPELINE
static uint32_t tune_clock(void){return h1_raw_tick_80hz();}
#endif
volatile unsigned minigpt_cache_mode;
volatile unsigned minigpt_cached_layers;
volatile unsigned minigpt_cache_warming;
volatile unsigned minigpt_cache_warm_bytes;
volatile unsigned minigpt_cache_ui[4]; /* presented layer, completed layers, bytes, percent */
#endif
static h1_file *model_file,*log_file;
static uint16_t pixels[480*272];
static char input[768],answer[8192],notice[100];
static int answer_len,preset_mode,preset_index,quit,cancel;
#define CHAT_TURNS 6
#define CHAT_TOP 39
#define CHAT_BOTTOM 198
#define CHAT_VIEW (CHAT_BOTTOM-CHAT_TOP)
static struct {char question[768],reply[8192];unsigned state;} chat[CHAT_TURNS];
volatile unsigned minigpt_chat_count;
volatile int minigpt_chat_scroll,minigpt_chat_height;
static int follow_tail=1,clip_top,clip_bottom=272;
static int pen_target,pen_y,pen_last_y,pen_drag;
#ifdef MG_DIAGNOSTICS
/* Collect in RAM; no log writes inside DMA callbacks or per-token inference. */
static struct {uint32_t number,id,tick;} token_log[96];
static struct {uint64_t bytes;uint32_t chunks,seeks,error,offset,wanted,actual,state;} io_diag;
static uint32_t round_number;
static void io_error_log(void)
{if(io_diag.error)h1_diag("IO_ERROR kind=%u offset=%u wanted=%u actual=%u state=%u",io_diag.error,io_diag.offset,io_diag.wanted,io_diag.actual,io_diag.state);}
#endif
static void key(int k);
static void touch_event(int code,int busy);
static uint32_t last_draw;
extern uint32_t h1_wall_clock(void);
static const char *presets[]={"你好，请简单介绍一下自己。","什么是人工智能？","请讲一个简短的故事。","为什么天空是蓝色的？","请介绍一下中国。","怎样学好数学？","请写一首关于春天的诗。","一加一等于几？","请解释什么是计算机。","请用中文回答：什么是学习？"};

int h1_diag_is_verbose(void){return 0;}
void h1_diag(const char *format,...)
{
    if(!log_file)return;char line[256];va_list args;va_start(args,format);int n=vsnprintf(line,sizeof line-2,format,args);va_end(args);
    if(n<0)return;if(n>(int)sizeof line-3)n=sizeof line-3;line[n++]='\n';h1_fwrite(line,1,(unsigned)n,log_file);
}
static void rect(int x,int y,int w,int h,uint16_t c)
{for(int r=y>clip_top?y:clip_top;r<y+h && r<clip_bottom;r++)for(int col=x;col<x+w && col<480;col++)if(r>=0 && col>=0)pixels[r*480+col]=c;}
static void glyph(unsigned cp,int x,int y,uint16_t color)
{
    unsigned lo=0,hi=sizeof glyphs/sizeof glyphs[0];
    while(lo<hi){unsigned mid=(lo+hi)/2;if(glyphs[mid].cp<cp)lo=mid+1;else hi=mid;}
    if(lo>=sizeof glyphs/sizeof glyphs[0] || glyphs[lo].cp!=cp){rect(x,y+3,7,1,color);rect(x,y+13,7,1,color);rect(x,y+3,1,11,color);rect(x+6,y+3,1,11,color);return;}
    unsigned width=glyphs[lo].width;
    for(unsigned r=0;r<16;r++)for(unsigned col=0;col<width;col++)if(glyphs[lo].rows[r]&(1u<<(width-1-col)))rect(x+(int)col,y+(int)r,1,1,color);
}
static void text(const char *s,int x,int y,uint16_t color)
{
    while(*s && x<470){int cp=mg_utf8(&s);if(cp<0)break;glyph((unsigned)cp,x,y,color);x+=cp<128?8:16;}
}
/* The same text layout measures and paints bubbles, including explicit lines. */
static int bubble_text(const char *s,int x,int y,int max_width,uint16_t color,int paint,int *widest)
{
    int col=0,line=0,wide=0;
    while(*s){int cp=mg_utf8(&s);if(cp<0)break;int w=cp<128?8:16;
        if(cp=='\r')continue;if(cp=='\t')cp=' ';
        if(cp=='\n' || col+w>max_width){if(col>wide)wide=col;line++;col=0;if(cp=='\n')continue;}
        if(paint && y+line*18+16>clip_top && y+line*18<clip_bottom)glyph((unsigned)cp,x+col,y+line*18,color);
        col+=w;
    }
    if(col>wide)wide=col;if(widest)*widest=wide;return line+1;
}
static void rounded(int x,int y,int w,int h,int radius,uint16_t color)
{
    static const unsigned char edge[8]={6,4,2,1,1,0,0,0};
    int top=y>clip_top?y:clip_top,bottom=y+h<clip_bottom?y+h:clip_bottom;
    for(int r=top;r<bottom;r++){int d=r-y;if(y+h-1-r<d)d=y+h-1-r;int inset=d<radius?edge[d*8/radius]:0;rect(x+inset,r,w-inset*2,1,color);}
}
static void circle(int x,int y,int radius,uint16_t color)
{for(int row=-radius;row<=radius;row++)for(int col=-radius;col<=radius;col++)if(row*row+col*col<=radius*radius)rect(x+col,y+row,1,1,color);}
static const char *reply_text(unsigned i)
{
    unsigned state=chat[i].state;
    const char *s=(i+1==minigpt_chat_count && (state==2 || state==3))?answer:chat[i].reply;
    if(s[0])return s;
    return state==2?"正在思考…":state==3?"正在生成…":state==6?"已停止生成。":state==5?"生成失败，请重试。":"未生成回答。";
}
static int bubble(const char *s,int user,int y,int paint)
{
    int widest,lines=bubble_text(s,0,0,user?320:368,0,0,&widest);
    int width=widest+24;if(width<80)width=80;int h=lines*18+16,x=user?456-width:36;
    if(paint && y+h>CHAT_TOP && y<CHAT_BOTTOM){
        rounded(x,y,width,h,10,user?0xe75c:0xdedb);
        if(!user){rounded(x+1,y+1,width-2,h-2,9,0xffff);circle(19,y+13,9,0x04d0);glyph('M',15,y+5,0xffff);}
        bubble_text(s,x+12,y+8,user?320:368,0x2124,1,0);
    }
    return h+10;
}
static void draft_line(const char *s,int x,int y,int max_width,uint16_t color)
{
    int width=0;while(*s){int cp=mg_utf8(&s);if(cp<0)break;if(cp=='\n' || cp=='\r')cp=' ';
        int w=cp<128?8:16;if(width+w>max_width-24){text("...",x+width,y,color);break;}glyph((unsigned)cp,x+width,y,color);width+=w;}
}
static int chat_height(void)
{int h=8;for(unsigned i=0;i<minigpt_chat_count;i++){h+=bubble(chat[i].question,1,0,0);h+=bubble(reply_text(i),0,0,0);}return h;}
static void scroll_chat(int delta)
{
    int max=chat_height()-CHAT_VIEW;if(max<0)max=0;minigpt_chat_scroll+=delta;
    if(minigpt_chat_scroll<0)minigpt_chat_scroll=0;if(minigpt_chat_scroll>max)minigpt_chat_scroll=max;
    follow_tail=minigpt_chat_scroll==max;
}
static void new_chat(void)
{
    minigpt_chat_count=0;minigpt_chat_scroll=0;answer_len=0;answer[0]=input[0]=notice[0]=0;
#ifdef MG_PREFIX_CACHE
    mg_reset(&model);minigpt_prefill_reused=0;
#endif
    preset_mode=0;follow_tail=1;if(model.work)minigpt_status.state=1;
}
static void push_question(void)
{
    if(minigpt_chat_count==CHAT_TURNS){memmove(chat,chat+1,sizeof chat[0]*(CHAT_TURNS-1));minigpt_chat_count--;}
    unsigned i=minigpt_chat_count++;strcpy(chat[i].question,input);chat[i].reply[0]=0;chat[i].state=2;
    input[0]=0;follow_tail=1;
}
static void present(void)
{
    typedef volatile uint32_t *(*buffer_fn)(uint16_t *,void *);
    buffer_fn fn=(buffer_fn)h1_runtime_entry(h1_runtime_table(H1_RUNTIME_GUI_TABLE_SLOT),0x8f4);
    uint16_t info[6]={0};volatile uint32_t *out=fn?fn(info,0):0;
    if(!out || info[1]!=480 || info[2]!=272 || info[3]!=1920 || info[5]!=32)return;
    for(unsigned i=0;i<480*272;i++){uint32_t c=pixels[i];out[i]=((c&0xf800)<<8)|((c&0x7e0)<<5)|((c&31)<<3);}
}
static void draw(void)
{
    mg_perf_enter(MG_PERF_UI);
#ifdef MG_LAYER_CACHE
    unsigned ui_layer=0,ui_layers=0,ui_bytes=0,ui_percent=0;
#endif
    char line[128];clip_top=0;clip_bottom=272;rect(0,0,480,272,0xffff);
    circle(23,17,10,0x04d0);glyph('M',19,9,0xffff);
#ifdef MG_INT4
    text("MiniGPT4",41,9,0x2124);
#else
    text("MiniGPT",41,9,0x2124);
#endif
    text("离线",264,9,0x6b6d);rounded(314,4,57,27,8,0xf79e);text("例题",326,9,0x4a69);
    rounded(379,4,89,27,8,0xf79e);text("新聊天",399,9,0x2124);rect(0,35,480,1,0xef7d);
    clip_top=CHAT_TOP;clip_bottom=CHAT_BOTTOM;
    minigpt_chat_height=chat_height();int max=minigpt_chat_height-CHAT_VIEW;if(max<0)max=0;
    if(follow_tail)minigpt_chat_scroll=max;if(minigpt_chat_scroll>max)minigpt_chat_scroll=max;
    if(!minigpt_chat_count){
        circle(240,75,18,0x04d0);glyph('M',236,67,0xffff);
#ifdef MG_LAYER_CACHE
        if(minigpt_status.state==0){
            unsigned done,total;
            if(model.layer_cache_warming){
                ui_layer=model.layer_cache_warming;ui_layers=model.cached_layers;
                done=minigpt_cache_warm_bytes;total=model.layer_bytes;
                text("正在缓存权重",192,104,0x2124);
                snprintf(line,sizeof line,"已缓存 %u 层，正在读入第 %u 层",ui_layers,ui_layer);
            }else if(model.cache_tune_phase){
                done=model.cache_tune_sample*MG_LAYERS+minigpt_status.layer;total=2*MG_LAYERS;
                text("正在优化读取",192,104,0x2124);
                snprintf(line,sizeof line,"比较缓存方式 %u/2，测试 %u/2",model.cache_tune_phase,model.cache_tune_sample+1);
            }else {
                done=minigpt_status.load_bytes;total=MG_MODEL_BYTES;
                text("正在校验模型",192,104,0x2124);
                strcpy(line,"请稍候，校验后继续准备权重缓存");
            }
            draft_line(line,96,130,320,0x6b6d);
            if(done>total)done=total;
            unsigned percent=total?done*100u/total:0;
            unsigned fill=ui_layer && total?done*320u/total:percent*320u/100u;
            rounded(80,154,320,8,4,0xe75c);if(fill)rect(80,154,(int)fill,8,0x04d0);
            if(model.cache_tune_phase && !ui_layer)snprintf(line,sizeof line,"%u%%",percent);
            else snprintf(line,sizeof line,"%u%%    %u / %u KB",percent,done/1024u,total/1024u);
            draft_line(line,144,171,256,0x6b6d);
            if(ui_layer){ui_bytes=done;ui_percent=percent;}
        }else
#endif
        {text("有什么可以帮你？",176,108,0x2124);text("点击输入框，开始聊天",160,136,0x6b6d);}
    }else{
        int y=CHAT_TOP+8-minigpt_chat_scroll;
        for(unsigned i=0;i<minigpt_chat_count;i++){y+=bubble(chat[i].question,1,y,1);y+=bubble(reply_text(i),0,y,1);}
        if(max){int h=CHAT_VIEW*CHAT_VIEW/minigpt_chat_height;if(h<14)h=14;rect(474,CHAT_TOP+(CHAT_VIEW-h)*minigpt_chat_scroll/max,3,h,0xc618);}
    }
    clip_top=0;clip_bottom=272;
    int busy=minigpt_status.state==2 || minigpt_status.state==3;
#ifdef MG_LAYER_CACHE
    if(minigpt_status.state==0 && (model.layer_cache_warming || model.cache_tune_phase))busy=1;
#endif
#ifdef MG_INT4
    if(minigpt_status.state==0)snprintf(line,sizeof line,"校验 INT4 %u / %u KB",minigpt_status.load_bytes/1024,MG_MODEL_BYTES/1024);
#else
    if(minigpt_status.state==0){
#ifdef MG_LAYER_CACHE
        if(model.layer_cache_warming)snprintf(line,sizeof line,"模型校验已完成，点击方块跳过剩余缓存");
        else if(model.cache_tune_phase)snprintf(line,sizeof line,"正在选择更快的缓存方式，点击方块跳过");
        else
#endif
        snprintf(line,sizeof line,"加载模型 %u / 25484 KB",minigpt_status.load_bytes/1024);
    }
#endif
    else if(minigpt_status.state==2)snprintf(line,sizeof line,"正在思考 %u/%u · 点击方块停止",minigpt_status.prefill,minigpt_status.input_tokens);
    else if(minigpt_status.state==3)snprintf(line,sizeof line,"正在生成 %u · 点击方块停止",minigpt_status.generated);
    else if(preset_mode)snprintf(line,sizeof line,"例题 %d：%s",preset_index+1,presets[preset_index]);
    else if(notice[0])snprintf(line,sizeof line,"%s",notice);
    else {
#ifdef MG_LAYER_CACHE
        if(model.resident_mode)strcpy(line,"模型已就绪，全权重常驻");
        else snprintf(line,sizeof line,"模型已就绪，已缓存 %u 层",model.cached_layers);
#else
        strcpy(line,"MiniMind2-Small");
#endif
    }
    draft_line(line,12,201,456,0x6b6d);
    rounded(12,222,404,31,12,0xe75c);rounded(13,223,402,29,11,0xf79e);
    draft_line(input[0]?input:"输入消息…",24,230,376,input[0]?0x2124:0x8410);
    circle(444,238,16,busy || input[0]?0x04d0:0xc618);
    if(busy)rect(439,233,10,10,0xffff);
    else{rect(443,232,2,13,0xffff);for(int i=0;i<6;i++){rect(444-i,232+i,1,1,0xffff);rect(444+i,232+i,1,1,0xffff);}}
    text("E 编辑  回车发送  上下翻页  N 新聊天",76,256,0x8410);
    present();last_draw=h1_raw_tick_80hz();
#ifdef MG_LAYER_CACHE
    minigpt_cache_ui[0]=ui_layer;minigpt_cache_ui[1]=ui_layers;minigpt_cache_ui[2]=ui_bytes;minigpt_cache_ui[3]=ui_percent;
#endif
    mg_perf_leave();
}
static int interrupted(void)
{
    mg_perf_enter(MG_PERF_UI);
    for(int i=0;i<16;i++){int code=-1,key=-1;h1_event_fetch(&code,&key);if(code==-1 && key==-1)break;
#ifdef MG_LAYER_CACHE
        /* During required validation the send control is not a stop button.
         * Only optional cache warmup exposes a visible skip control. */
        if(minigpt_status.state!=0 || model.layer_cache_warming || model.cache_tune_phase)touch_event(code,1);
#else
        touch_event(code,1);
#endif
        if(code==H1_EVENT_KEY_DOWN && (key==H1_KEY_UP || key==H1_KEY_PAGE_UP)){scroll_chat(-36);draw();}
        if(code==H1_EVENT_KEY_DOWN && (key==H1_KEY_DOWN || key==H1_KEY_PAGE_DOWN)){scroll_chat(36);draw();}
        if(code==H1_EVENT_KEY_DOWN && (key==H1_KEY_ESCAPE || key==H1_KEY_BACK)){cancel=1;if(key==H1_KEY_BACK)quit=1;}
    }
    mg_perf_leave();return cancel;
}
static int yield(void *ui,int layer,int phase)
{(void)ui;(void)phase;minigpt_status.layer=(unsigned)layer;
#ifdef MG_PREFIX_CACHE
 minigpt_prefill_reused=(unsigned)model.prompt_reused;
 if(minigpt_status.state==2 && !model.prompt_total)minigpt_status.prefill=minigpt_prefill_reused;
#endif
 if(model.prompt_total)minigpt_status.prefill=(unsigned)(model.prompt_done/8+1
#ifdef MG_PREFIX_CACHE
 +model.prompt_reused
#endif
 );
 if(h1_raw_tick_80hz()-last_draw>=8)draw();return interrupted();}
static int readat(void *io,uint32_t off,void *p,uint32_t bytes)
{
#ifdef MG_LAYER_CACHE
    unsigned cache_phase_changed=minigpt_cache_warming!=model.layer_cache_warming;
    minigpt_cache_warming=model.layer_cache_warming;
    minigpt_cache_warm_bytes=0;
    if(minigpt_status.state==0 && cache_phase_changed)draw();
#endif
    h1_file *f=io;
#ifdef MG_LAYER_CACHE
    int need_seek=!reader_valid || reader_offset!=off;
#else
    int need_seek=1;
#endif
    int seek=need_seek?h1_fseek(f,(int)off,H1_SEEK_SET):(int)off;
#ifdef MG_LAYER_CACHE
    reader_valid=seek==(int)off;reader_offset=off;
#endif
#ifdef MG_DIAGNOSTICS
    io_diag.seeks+=(unsigned)need_seek;
    if(seek!=(int)off){io_diag.error=1;io_diag.offset=off;io_diag.wanted=bytes;io_diag.actual=(uint32_t)seek;io_diag.state=minigpt_status.state;return 0;}
#else
    if(seek!=(int)off)return 0;
#endif
    uint8_t *out=p;uint32_t done=0;
    while(bytes){unsigned n=bytes>65536?65536:bytes;
        unsigned actual=h1_fread(out,1,n,f);
#ifdef MG_LAYER_CACHE
        reader_offset+=actual;if(actual!=n)reader_valid=0;
#endif
#ifdef MG_DIAGNOSTICS
        io_diag.chunks++;io_diag.bytes+=actual;
        if(actual!=n){io_diag.error=2;io_diag.offset=off+done;io_diag.wanted=n;io_diag.actual=actual;io_diag.state=minigpt_status.state;return 0;}
#else
        if(actual!=n)return 0;
#endif
        out+=n;bytes-=n;done+=n;
#ifdef MG_LAYER_CACHE
        if(model.layer_cache_warming)minigpt_cache_warm_bytes=done;
#endif
        if(minigpt_status.state==0){if(off+done>minigpt_status.load_bytes)minigpt_status.load_bytes=off+done;if(h1_raw_tick_80hz()-last_draw>=8)draw();if(interrupted()){
#ifdef MG_LAYER_CACHE
            h1_diag("LOAD_INTERRUPTED cache_layer=%u offset=%u done=%u cancel=%u quit=%u",model.layer_cache_warming,off,done,(unsigned)cancel,(unsigned)quit);
#endif
            return 0;
        }}
        else if(minigpt_status.state==2 || minigpt_status.state==3){if(interrupted())return 0;}
    }
    return 1;
}
static int valid_input(const char *s)
{while(*s){int c=mg_utf8(&s);if(c<0 || (c<32 && c!='\n' && c!='\t' && c!='\r'))return 0;}return 1;}
static void load_prompt(void)
{
    h1_file *f=h1_fopen(MG_DATA_DIR "prompt.txt","rb");char tmp[sizeof input];
    if(!f){strcpy(notice,"找不到 MiniGPT/prompt.txt");return;}
    unsigned n=h1_fread(tmp,1,sizeof tmp-1,f);char extra;unsigned more=h1_fread(&extra,1,1,f);h1_fclose(f);tmp[n]=0;
    char *s=tmp;if(n>=3 && (uint8_t)s[0]==0xef && (uint8_t)s[1]==0xbb && (uint8_t)s[2]==0xbf)s+=3;
    if(more || strlen(tmp)!=n || !valid_input(s) || !*s){strcpy(notice,"prompt.txt 需为 UTF-8，最多767字节");return;}
    strcpy(input,s);notice[0]=0;preset_mode=0;
}
static void generate(void)
{
    char prompt[1024];int ids[MG_CONTEXT];
    if(!input[0])return;
    /* User-entered control tokens must remain ordinary text: reject rather than
     * allowing them to change the role delimiters in the generated template. */
    for(unsigned i=0;input[i];i++)if(!strncmp(input+i,"<|",2)){strcpy(notice,"提问不能含有模型控制标记");return;}
    snprintf(prompt,sizeof prompt,"<|im_start|>system\nYou are a helpful assistant<|im_end|>\n<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n",input);
    int n=mg_encode(prompt,ids,160);if(n<=0){strcpy(notice,"提问太长：含模板最多160个词元");return;}
    minigpt_status.input_tokens=(unsigned)n;minigpt_status.prefill=0;minigpt_status.generated=0;minigpt_status.state=2;
    uint32_t start_seconds=h1_wall_clock();
#ifdef MG_DIAGNOSTICS
    uint32_t prefill_end=0,first_token=0,mxu_start[5];uint64_t bytes_start=io_diag.bytes;
#ifdef MG_LAYER_CACHE
    uint32_t cache_hits_start=model.layer_cache_hits,cache_misses_start=model.layer_cache_misses;
#endif
    uint32_t chunks_start=io_diag.chunks,seeks_start=io_diag.seeks;io_diag.error=0;round_number++;
    for(unsigned i=0;i<5;i++)mxu_start[i]=mg_mxu_status[i];
#ifdef MG_DMA_PIPELINE
    uint32_t dma_start[8],hook_start[8];
    for(unsigned i=0;i<8;i++){dma_start[i]=mg_dma_status[i];hook_start[i]=mg_dma_hook_status[i];}
#endif
    h1_diag("ROUND_BEGIN id=%u prompt_tokens=%u resident=%u rtc=%u",round_number,(unsigned)n,(unsigned)model.resident_mode,start_seconds);
#endif
    mg_perf_begin();minigpt_status.start_tick=h1_raw_tick_80hz();answer_len=0;answer[0]=0;notice[0]=0;cancel=0;push_question();
#if !defined(MG_PREFIX_CACHE) || defined(MG_FULL_PREFILL)
    mg_reset(&model);
#endif
    draw();
    float *logits=0;
#ifndef MG_FULL_PREFILL
#ifdef MG_PREFIX_CACHE
    logits=mg_prompt_cached(&model,ids,n);minigpt_prefill_reused=(unsigned)model.prompt_reused;
#else
    logits=mg_prompt(&model,ids,n);
#endif
    if(logits)minigpt_status.prefill=(unsigned)n;
#else
    for(int i=0;i<n;i++){
        minigpt_status.prefill=(unsigned)i+1;
        logits=mg_forward(&model,ids[i]);if(!logits)break;
    }
#endif
#ifdef MG_DIAGNOSTICS
    prefill_end=h1_raw_tick_80hz();
#endif
    mg_perf_decode();
    if(logits){minigpt_status.state=3;chat[minigpt_chat_count-1].state=3;}
    for(int i=0;logits && !cancel && i<96 && model.position<MG_CONTEXT;i++){
        int token=mg_argmax(logits);minigpt_status.last_token=(unsigned)token;
        if(token==0 || token==2)break;
        int len;const uint8_t *piece=mg_piece(token,&len);
        if(answer_len+len>=(int)sizeof answer-1){strcpy(notice,"回答已达到文本缓冲区上限");break;}
        memcpy(answer+answer_len,piece,(unsigned)len);answer_len+=len;answer[answer_len]=0;
        minigpt_status.generated++;
#ifdef MG_DIAGNOSTICS
        unsigned t=minigpt_status.generated-1;token_log[t].number=t+1;token_log[t].id=(unsigned)token;token_log[t].tick=h1_raw_tick_80hz();if(!t)first_token=token_log[t].tick;
#else
        h1_diag("TOKEN %u id=%d tick=%u",minigpt_status.generated,token,h1_raw_tick_80hz());
#endif
        draw();
        if(interrupted())break;
        if(i<95 && model.position<MG_CONTEXT)logits=mg_forward(&model,token);
    }
    minigpt_status.end_tick=h1_raw_tick_80hz();
#ifdef MG_DIAGNOSTICS
    uint32_t finish_rtc=h1_wall_clock();
#endif
    mg_perf_end();
#ifdef MG_DIAGNOSTICS
    for(unsigned i=0;i<minigpt_status.generated;i++)h1_diag("TOKEN %u id=%u tick=%u",token_log[i].number,token_log[i].id,token_log[i].tick);
    h1_diag("ROUND_TIME id=%u prefill_ticks=%u decode_ticks=%u first_token_ticks=%u first_token_valid=%u rtc_elapsed=%u",round_number,prefill_end-minigpt_status.start_tick,minigpt_status.end_tick-prefill_end,minigpt_status.generated?first_token-minigpt_status.start_tick:0,minigpt_status.generated!=0,finish_rtc-start_seconds);
#ifdef MG_PREFIX_CACHE
    h1_diag("PREFILL_REUSE tokens=%u computed=%u",(unsigned)model.prompt_reused,(unsigned)n-(unsigned)model.prompt_reused);
#endif
    h1_diag("ROUND_IO bytes=%llu chunks=%u seeks=%u",(unsigned long long)(io_diag.bytes-bytes_start),io_diag.chunks-chunks_start,io_diag.seeks-seeks_start);io_error_log();
#ifdef MG_LAYER_CACHE
    h1_diag("LAYER_CACHE_ROUND layers=%u hits=%u misses=%u avoided_bytes=%llu",model.cached_layers,model.layer_cache_hits-cache_hits_start,model.layer_cache_misses-cache_misses_start,(unsigned long long)(model.layer_cache_hits-cache_hits_start)*model.layer_bytes);
#endif
    h1_diag("MXU_ROUND backend=%d calls=%u terms=%u scalar=%u errors=%u",(int32_t)mg_mxu_status[0],mg_mxu_status[1]-mxu_start[1],mg_mxu_status[2]-mxu_start[2],mg_mxu_status[3]-mxu_start[3],mg_mxu_status[4]-mxu_start[4]);
#ifdef MG_DMA_PIPELINE
    h1_diag("DMA_ROUND double_buffer=%u jobs=%u pending=%u overlap=%u fallback=%u irq_errors=%u swaps=%u read_errors=%u",model.layer_next!=0,mg_dma_status[1]-dma_start[1],mg_dma_status[2]-dma_start[2],mg_dma_status[3]-dma_start[3],mg_dma_status[4]-dma_start[4],mg_dma_status[5]-dma_start[5],mg_dma_status[6]-dma_start[6],mg_dma_status[7]-dma_start[7]);
    h1_diag("HOOK_ROUND ready=%u active=%u installs=%u restores=%u rejects=%u busy=%u conflicts=%u cache_syncs=%u",mg_dma_hook_status[0],mg_dma_hook_status[1],mg_dma_hook_status[2]-hook_start[2],mg_dma_hook_status[3]-hook_start[3],mg_dma_hook_status[4]-hook_start[4],mg_dma_hook_status[5]-hook_start[5],mg_dma_hook_status[6]-hook_start[6],mg_dma_hook_status[7]-hook_start[7]);
#endif
#endif
    if(cancel){minigpt_status.state=6;strcpy(notice,"已停止；下次发送会开始新提问");}
    else if(!logits){minigpt_status.state=5;strcpy(notice,"推理失败：检查模型文件或重新启动");}
    else {
        minigpt_status.state=4;uint32_t end_seconds=h1_wall_clock();
        if(start_seconds && end_seconds>=start_seconds && end_seconds-start_seconds<86400)
            snprintf(notice,sizeof notice,"已完成 %u词元 / 约%u秒",minigpt_status.generated,end_seconds-start_seconds);
        else snprintf(notice,sizeof notice,"已完成 %u词元",minigpt_status.generated);
    }
    /* Never display a trailing partial UTF-8 token. */
    const char *s=answer,*complete=answer;
    while(*s){if(mg_utf8(&s)<0)break;complete=s;}answer_len=(int)(complete-answer);answer[answer_len]=0;
    strcpy(chat[minigpt_chat_count-1].reply,answer);chat[minigpt_chat_count-1].state=minigpt_status.state;
    h1_diag("END state=%u tokens=%u ticks=%u",minigpt_status.state,minigpt_status.generated,minigpt_status.end_tick-minigpt_status.start_tick);
#ifdef MG_MXU
    h1_diag("MXU backend=%d calls=%u terms=%u scalar=%u errors=%u",(int32_t)mg_mxu_status[0],mg_mxu_status[1],mg_mxu_status[2],mg_mxu_status[3],mg_mxu_status[4]);
#endif
#ifdef MG_INT4
    h1_diag("Q4_PAIR groups=%u terms=%u fallback=%u",mg_q4_pair_status[0],mg_q4_pair_status[1],mg_q4_pair_status[2]);
#endif
    draw();
}
static void edit_input(void)
{
    typedef int (*window_fn)(void);
    window_fn close=(window_fn)h1_runtime_entry(h1_runtime_table(H1_RUNTIME_GUI_TABLE_SLOT),0x850);
    window_fn open=(window_fn)h1_runtime_entry(h1_runtime_table(H1_RUNTIME_GUI_TABLE_SLOT),0x84c);
    close();if(mg_native_input(input,sizeof input))notice[0]=0;open();draw();
}
static void key(int k)
{
    if(k==H1_KEY_BACK || k==H1_KEY_ESCAPE){if(preset_mode)preset_mode=0;else quit=1;return;}
    if(minigpt_status.state==5 && !model.work)return;
    if(k==H1_KEY_SYMBOL){preset_mode=!preset_mode;return;}
    if(preset_mode){
        unsigned n=sizeof presets/sizeof presets[0];
        if(k==H1_KEY_UP || k==H1_KEY_LEFT)preset_index=(preset_index+(int)n-1)%(int)n;
        if(k==H1_KEY_DOWN || k==H1_KEY_RIGHT)preset_index=(preset_index+1)%(int)n;
        if(k==H1_KEY_ENTER || k==H1_KEY_CONFIRM){strcpy(input,presets[preset_index]);preset_mode=0;notice[0]=0;}
        return;
    }
    if(k==H1_KEY_ENTER || k==H1_KEY_CONFIRM){generate();return;}
    if(k==H1_KEY_E){edit_input();return;}
    if(k==H1_KEY_N){new_chat();return;}
    if(k==H1_KEY_LEFT){input[0]=0;notice[0]=0;return;}
    if(k==H1_KEY_RIGHT){load_prompt();return;}
    if(k==H1_KEY_UP || k==H1_KEY_PAGE_UP){scroll_chat(-36);return;}
    if(k==H1_KEY_DOWN || k==H1_KEY_PAGE_DOWN){scroll_chat(36);return;}
}
/* Native game-window pen events: down/move=11, up=8; calibrated GUI+6C0. */
static int hit(int x,int y,int busy)
{
    if(y>=219 && y<=255)return x>=425?2:busy?0:1;
    if(y<35 && !busy){if(x>=379)return 3;if(x>=314)return 4;}
    return y>=CHAT_TOP && y<CHAT_BOTTOM?5:0;
}
static void touch_event(int code,int busy)
{
    int x,y;if(code!=11 && code!=8)return;
    if(!h1_touch_position(&x,&y)){if(code==8)pen_target=0;return;}
    if(code==11){
        if(!pen_target){pen_target=hit(x,y,busy);pen_y=pen_last_y=y;pen_drag=0;}
        else if(pen_target==5){int delta=pen_last_y-y;if(y-pen_y>6 || pen_y-y>6)pen_drag=1;if(pen_drag){scroll_chat(delta);draw();}pen_last_y=y;}
    }else{
        int target=pen_target;pen_target=0;
        /* V1.41 queues down/up but does not guarantee pen-move events. */
        if(target==5 && (pen_drag || y-pen_y>6 || pen_y-y>6)){
            scroll_chat(pen_last_y-y);pen_drag=1;draw();
        }
        if(!pen_drag && hit(x,y,busy)==target){
            if(target==2){if(busy)cancel=1;else key(H1_KEY_ENTER);}
            if(!busy){if(target==1)edit_input();if(target==3)new_chat();if(target==4)key(H1_KEY_SYMBOL);}
            draw();
        }
    }
}
int h1_app_main(void)
{
    typedef int (*window_fn)(void);
    window_fn open=(window_fn)h1_runtime_entry(h1_runtime_table(H1_RUNTIME_GUI_TABLE_SLOT),0x84c);
    window_fn close=(window_fn)h1_runtime_entry(h1_runtime_table(H1_RUNTIME_GUI_TABLE_SLOT),0x850);
    if(!open || !close || !open())return 1;
    for(int i=0;i<64;i++){int c=-1,k=-1;h1_event_fetch(&c,&k);if(c==-1 && k==-1)break;}
    minigpt_status.magic=0x4d475054;minigpt_status.state=0;strcpy(input,presets[0]);draw();
    log_file=h1_fopen(MG_DATA_DIR "minigpt.log","wb");
#ifdef MG_INT4
    h1_diag("MiniGPT 0.4.2 INT4 MiniMind2-Small Q4 MXU diagnostics");
#elif defined(MG_MXU)
    h1_diag("MiniGPT 0.3.6 MiniMind2-Small Q8 MXU diagnostics");
#else
    h1_diag("MiniGPT 0.1 MiniMind2-Small Q8");
#endif
#ifdef MG_DIAGNOSTICS
    uint32_t load_start=h1_raw_tick_80hz(),load_rtc=h1_wall_clock();h1_diag("DIAG tick_unit=raw rtc_unit=seconds token_log=buffered history=single_turn");
#endif
    model_file=h1_fopen(MG_DATA_DIR MG_MODEL_NAME,"rb");int error=-1;
#ifdef MG_LAYER_CACHE
    int actual_model_bytes=-1;
    if(!model_file)error=-4;
    else {
        actual_model_bytes=h1_fseek(model_file,0,H1_SEEK_END);
        if(actual_model_bytes!=(int)MG_MODEL_BYTES)error=-5;
        else error=mg_load(&model,readat,model_file,1);
    }
#ifdef MG_DMA_PIPELINE
    if(!error && !cancel && !quit){
        model.yield=yield;
        if(!mg_select_layer_cache(&model,tune_clock)){mg_close(&model);error=-1;}
        minigpt_cache_mode=model.cache_tune_selected;
        h1_diag("CACHE_MODE initial_layers=%u layers=%u dma_ticks=%u cache_ticks=%u samples=2 selected=%u canceled=%u",model.cache_tune_initial_layers,model.cached_layers,model.cache_tune_dma_ticks,model.cache_tune_sync_ticks,model.cache_tune_selected,model.cache_tune_canceled);
    }
#endif
    if(error && cancel)error=-3;
    h1_diag("LOAD_FILE opened=%u actual_bytes=%d expected_bytes=%u",model_file!=0,actual_model_bytes,MG_MODEL_BYTES);
    h1_diag("CACHE_WARMUP layers=%u read_aborts=%u canceled=%u quit=%u",model.cached_layers,model.layer_cache_read_aborts,(unsigned)cancel,(unsigned)quit);
    minigpt_cache_warming=0;
    minigpt_cache_warm_bytes=0;
#else
    if(model_file && h1_fseek(model_file,0,H1_SEEK_END)==(int)MG_MODEL_BYTES)error=mg_load(&model,readat,model_file,1);
#endif
    minigpt_status.error=(unsigned)error;minigpt_status.resident=(unsigned)model.resident_mode;
    h1_diag("LOAD result=%d resident=%d",error,model.resident_mode);
#ifdef MG_DIAGNOSTICS
    h1_diag("LOAD_SUMMARY ticks=%u rtc_elapsed=%u file_bytes=%u scanned_to=%u io_bytes=%llu chunks=%u seeks=%u",h1_raw_tick_80hz()-load_start,h1_wall_clock()-load_rtc,model.file_bytes,minigpt_status.load_bytes,(unsigned long long)io_diag.bytes,io_diag.chunks,io_diag.seeks);
    h1_diag("MEMORY resident=%u embedding_bytes=%u layer_bytes=%u kv_bytes=%u work_bytes=%u",(unsigned)model.resident_mode,model.embedding?model.t[0].bytes:0,model.layer?model.layer_bytes:0,model.cache?MG_LAYERS*MG_CONTEXT*128u*2u*4u:0,model.work?(512*5+128*2+1408*2+MG_CONTEXT+6400)*4u+1408*2u:0);
#ifdef MG_LAYER_CACHE
    minigpt_cached_layers=model.cached_layers;
    h1_diag("LAYER_CACHE layers=%u bytes=%u reserve_bytes=%u",model.cached_layers,model.cached_layers*model.layer_bytes,MG_LAYER_CACHE_RESERVE);
#endif
    io_error_log();
#endif
#ifdef MG_MXU
    h1_diag("MXU_INIT backend=%d errors=%u",(int32_t)mg_mxu_status[0],mg_mxu_status[4]);
#endif
#ifdef MG_LAYER_CACHE
    if(error){
        minigpt_status.state=5;
        strcpy(notice,error==-2?"内存不足：无法分配推理缓存":error==-3?"加载已取消，请重新打开程序":error==-4?"找不到根目录MiniGPT/model.mg8":error==-5?"model.mg8大小不符，需26096156字节":"模型校验或读取失败，请查看minigpt.log");
    }else {
        cancel=0;pen_target=0;minigpt_status.state=1;model.yield=yield;load_prompt();
        if(model.layer_cache_read_aborts)snprintf(notice,sizeof notice,"已跳过剩余缓存，已缓存 %u 层，可提问",model.cached_layers);
    }
#else
    if(error){minigpt_status.state=5;strcpy(notice,error==-2?"内存不足：无法分配推理缓存":"请安装完整的MiniGPT/model.mg8");}
    else {minigpt_status.state=1;model.yield=yield;load_prompt();}
#endif
    draw();
    while(!quit){int code=-1,k=-1;h1_event_fetch(&code,&k);
        if(code==H1_EVENT_KEY_DOWN || code==H1_EVENT_KEY_UP){
            unsigned trace=minigpt_key_events++&31u;minigpt_key_trace[trace*2]=code==H1_EVENT_KEY_DOWN;minigpt_key_trace[trace*2+1]=k;
            if(code==H1_EVENT_KEY_DOWN)key(k);draw();
        }else touch_event(code,0);
    }
    mg_close(&model);if(model_file)h1_fclose(model_file);if(log_file){h1_diag("EXIT");h1_fclose(log_file);}close();return 0;
}
