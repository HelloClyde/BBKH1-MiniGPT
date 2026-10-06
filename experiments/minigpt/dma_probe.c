/* Emulator-only experiment. This BDA never installs its own firmware hook.
 * The private harness installs/removes it while the app waits at phase gates.
 * Callback: integer RAM computation only, no firmware/GUI/file API calls. */
#include "h1_sdk.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define LAYER_BYTES 2840576u
#define LAYER0 3371548u
volatile struct {
    uint32_t magic,state,command,error,hook_calls,pending_calls,tiles,rows;
    uint32_t irq_bad,bytes_equal,dots_equal,cancel,crc_ref,crc_got,drained,rows_during_read,overlap_tiles;
} dma_probe_status;
volatile int32_t dma_probe_output[512];
static int32_t expected[512];
static int16_t activation[512];
static const int8_t *weights;
static unsigned row,col;
static int32_t partial;
static int running;
static uint8_t *current,*next,*reference;
static void tile(void)
{
    if(!running || row==512 || dma_probe_status.cancel)return;
    unsigned end=col+128;
    for(unsigned i=col;i<end;i++)partial+=(int32_t)weights[row*512+i]*activation[i];
    col=end;dma_probe_status.tiles++;
    if(col==512){dma_probe_output[row]=partial;row++;col=0;partial=0;dma_probe_status.rows=row;}
}
void dma_probe_work(void)
{
    if(!running)return;
    dma_probe_status.hook_calls++;
    uint32_t status;__asm__ volatile("mfc0 %0,$12":"=r"(status));
    if(status&1)dma_probe_status.irq_bad++;
    if(*(volatile uint32_t *)0xb30200a8u){
        dma_probe_status.pending_calls++;
        if(row<512 && !dma_probe_status.cancel){tile();dma_probe_status.overlap_tiles++;}
    }
}
static uint32_t crc(const uint8_t *p,unsigned bytes)
{
    uint32_t c=0xffffffffu;
    while(bytes--){c^=*p++;for(unsigned i=0;i<8;i++)c=(c>>1)^((c&1)?0xedb88320u:0);}
    return c^0xffffffffu;
}
static int read_layer(h1_file *f,unsigned layer,uint8_t *target)
{
    unsigned off=LAYER0+layer*LAYER_BYTES;
    if(h1_fseek(f,(int)off,H1_SEEK_SET)!=(int)off)return 0;
    for(unsigned done=0;done<LAYER_BYTES;){
        unsigned n=LAYER_BYTES-done;if(n>65536)n=65536;
        if(h1_fread(target+done,1,n,f)!=n)return 0;
        done+=n;
    }
    return 1;
}
static void wait_command(unsigned command)
{
    while(dma_probe_status.command!=command){int code=-1,key=-1;h1_event_fetch(&code,&key);}
}
static void fail(unsigned error)
{dma_probe_status.error=error;dma_probe_status.state=99;wait_command(9);}
int h1_app_main(void)
{
    dma_probe_status.magic=0x444d4150u;
    h1_file *f=h1_fopen("A:\\MiniGPT\\model.mg8","rb");
    if(!f){fail(1);return 0;}
    current=malloc(LAYER_BYTES);next=malloc(LAYER_BYTES);reference=malloc(LAYER_BYTES);
    if(!current || !next || !reference){fail(2);goto exit;}
    if(!read_layer(f,0,current) || !read_layer(f,1,reference)){fail(3);goto exit;}
    weights=(const int8_t *)(current+4096); /* layer0 Wq: RMS 2048 + scales 2048 */
    for(unsigned i=0;i<512;i++)activation[i]=(int16_t)((i*37u)%8191u)-4095;
    for(unsigned r=0;r<512;r++){
        int32_t sum=0;for(unsigned i=0;i<512;i++)sum+=(int32_t)weights[r*512+i]*activation[i];
        expected[r]=sum;
    }
    dma_probe_status.crc_ref=crc(reference,LAYER_BYTES);
    dma_probe_status.state=1;wait_command(1); /* harness installs hook here */
    running=1;dma_probe_status.state=2;
    int ok=read_layer(f,1,next);running=0;
    dma_probe_status.rows_during_read=row;
    dma_probe_status.drained=(*(volatile uint32_t *)0xb30200a8u==0);
    dma_probe_status.bytes_equal=ok && !memcmp(next,reference,LAYER_BYTES);
    dma_probe_status.crc_got=crc(next,LAYER_BYTES);
    running=1;while(row<512)tile();running=0;
    dma_probe_status.dots_equal=!memcmp((const void *)dma_probe_output,expected,sizeof expected);
    dma_probe_status.state=3;wait_command(2);
    /* Cancel only the compute job; the synchronous reader must drain safely. */
    dma_probe_status.cancel=1;row=col=0;partial=0;
    unsigned before=dma_probe_status.tiles;
    running=1;ok=read_layer(f,1,next);running=0;
    if(!ok || memcmp(next,reference,LAYER_BYTES) || dma_probe_status.tiles!=before ||
       *(volatile uint32_t *)0xb30200a8u){fail(4);goto exit;}
    dma_probe_status.state=4;wait_command(9); /* harness restores before close */
exit:
    h1_fclose(f);free(current);free(next);free(reference);
    dma_probe_status.state=5;return 0;
}
void h1_diag(const char *format,...){(void)format;}
int h1_diag_is_verbose(void){return 0;}
