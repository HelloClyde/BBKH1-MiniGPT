#include "tokenizer.h"
#include <string.h>
#include "token_data.h"
int mg_utf8(const char **input)
{
    const unsigned char *s=(const unsigned char *)*input;unsigned c=*s++,n=0,min=0;
    if(c<128){*input=(const char *)s;return (int)c;}
    if(c>=0xc2 && c<0xe0){c&=31;n=1;min=128;}
    else if(c>=0xe0 && c<0xf0){c&=15;n=2;min=2048;}
    else if(c>=0xf0 && c<=0xf4){c&=7;n=3;min=65536;}
    else return -1;
    while(n--){if((*s&0xc0)!=0x80)return -1;c=(c<<6)|(*s++&63);}
    if(c<min || c>0x10ffff || (c>=0xd800 && c<=0xdfff))return -1;
    *input=(const char *)s;return (int)c;
}
static int kind(int c)
{
    unsigned lo=0,hi=sizeof categories/sizeof categories[0];
    while(lo<hi){unsigned mid=(lo+hi)/2;if(categories[mid].last<(unsigned)c)lo=mid+1;else hi=mid;}
    return lo<sizeof categories/sizeof categories[0] && categories[lo].first<=(unsigned)c?categories[lo].kind:0;
}
static int merge(int a,int b,int *rank)
{
    uint32_t key=1u+(unsigned)a+((unsigned)b<<13),slot=(key*2654435761u)>>18;
    while(merge_keys[slot]){
        if(merge_keys[slot]==key){*rank=merge_ranks[slot];return merge_values[slot];}
        slot=(slot+1)&16383;
    }
    *rank=0x7fffffff;return -1;
}
static int segment(const char *s,int bytes,int *out,int cap,int count)
{
    int parts[1024];if(bytes>1024)return -1;
    for(int i=0;i<bytes;i++)parts[i]=byte_tokens[(uint8_t)s[i]];
    int n=bytes;
    while(n>1){
        int best=-1,best_rank=0x7fffffff,value=0;
        for(int i=0;i<n-1;i++){int rank,id=merge(parts[i],parts[i+1],&rank);if(rank<best_rank){best=i;best_rank=rank;value=id;}}
        if(best<0)break;
        parts[best]=value;memmove(parts+best+1,parts+best+2,(n-best-2)*sizeof(int));n--;
    }
    if(count+n>cap)return -1;memcpy(out+count,parts,n*sizeof(int));return count+n;
}
int mg_encode(const char *s,int *out,int cap)
{
    int count=0;
    while(*s){
        static const char *special[]={"<|endoftext|>","<|im_start|>","<|im_end|>"};int found=0;
        for(int i=0;i<3;i++){unsigned len=(unsigned)strlen(special[i]);if(!strncmp(s,special[i],len)){if(count>=cap)return -1;out[count++]=i;s+=len;found=1;break;}}
        if(found)continue;
        const char *start=s,*p=s;int c=mg_utf8(&p);if(c<0)return -1;
        int cat=kind(c);
        if(c=='\''){
            static const char *suffix[]={"s","t","re","ve","m","ll","d"};
            for(unsigned i=0;i<7;i++){unsigned n=(unsigned)strlen(suffix[i]);if(!strncmp(p,suffix[i],n)){p+=n;found=1;break;}}
        }
        if(!found){
            if(c==' ' && *p && strncmp(p,"<|im_start|>",12) && strncmp(p,"<|im_end|>",10) && strncmp(p,"<|endoftext|>",13)){
                const char *next=p;int cp=mg_utf8(&next);if(cp<0)return -1;if(kind(cp)!=3){cat=kind(cp);p=next;}
            }
            if(cat==3){
                const char *last=p;
                while(*p){const char *next=p;int cp=mg_utf8(&next);if(cp<0)return -1;if(kind(cp)!=3)break;last=p;p=next;}
                /* \s+(?!\S) leaves the final whitespace for the next segment. */
                if(*p && p!=last && last!=start)p=last;
            }else{
                while(*p){
                    if(!strncmp(p,"<|im_start|>",12) || !strncmp(p,"<|im_end|>",10) || !strncmp(p,"<|endoftext|>",13))break;
                    const char *next=p;int cp=mg_utf8(&next);if(cp<0)return -1;if(kind(cp)!=cat)break;p=next;
                }
            }
        }
        count=segment(start,(int)(p-start),out,cap,count);if(count<0)return -1;s=p;
    }
    return count;
}
const uint8_t *mg_piece(int token,int *length)
{
    if(token<0 || token>=6400){*length=0;return 0;}
    *length=(int)(token_offsets[token+1]-token_offsets[token]);return token_bytes+token_offsets[token];
}
