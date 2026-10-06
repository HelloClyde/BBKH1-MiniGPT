"""Summarize MiniGPT device logs without assuming the raw tick rate."""
from pathlib import Path
import argparse,json,re

def fields(line):
    return {k:int(v,16) if k in ('cpccr','cppcr','clkgr') else int(v)
            for k,v in re.findall(r'(\w+)=(-?[0-9a-fA-F]+)(?=\s|$)',line)}

def analyze(text):
    result={'load':{},'memory':{},'rounds':[],'notes':[
        'raw_ticks are firmware timer units; no fixed 40/80 Hz assumption',
        'read time includes filesystem, ECC and DMA callback computation',
        'DMA overlap counts tiles started while DMA was pending, not saved time',
        'token records are buffered and written after inference; exit closes the log']}
    current=None
    for line in text.splitlines():
        if line.startswith('LOAD_SUMMARY '):result['load']=fields(line)
        elif line.startswith('LOAD_FILE '):result['load_file']=fields(line)
        elif line.startswith('CACHE_WARMUP '):result['cache_warmup']=fields(line)
        elif line.startswith('LOAD_INTERRUPTED '):result.setdefault('load_interruptions',[]).append(fields(line))
        elif line.startswith('MEMORY '):result['memory']=fields(line)
        elif line.startswith('CACHE_MODE '):result['cache_mode']=fields(line)
        elif line.startswith('LAYER_CACHE '):result['layer_cache']=fields(line)
        elif line.startswith('ROUND_BEGIN '):
            current={'begin':fields(line),'stages':{},'phases':{'prefill':{},'decode':{}},'tokens':[],'complete':False}
            result['rounds'].append(current)
        elif current is not None:
            if line.startswith('PERF ') and 'raw_ticks=' in line:
                current['stages'][line.split()[1]]=fields(line)
            elif line.startswith('PERF bytes='):current['perf']=fields(line)
            elif line.startswith('PHASE '):
                parts=line.split();phase=current['phases'][parts[1]]
                if parts[2].startswith('bytes='):phase['bytes']=fields(line)['bytes']
                else:phase[parts[2]]=fields(line)
            elif line.startswith('TOKEN '):current['tokens'].append({'number':int(line.split()[1]),**fields(line)})
            elif line.startswith('CLOCK '):current['clock']=fields(line)
            elif line.startswith('ROUND_TIME '):current['time']=fields(line)
            elif line.startswith('PREFILL_REUSE '):current['prefill_reuse']=fields(line)
            elif line.startswith('ROUND_IO '):current['io']=fields(line)
            elif line.startswith('LAYER_CACHE_ROUND '):current['layer_cache']=fields(line)
            elif line.startswith('MXU_ROUND '):current['mxu']=fields(line)
            elif line.startswith('DMA_ROUND '):current['dma']=fields(line)
            elif line.startswith('HOOK_ROUND '):current['hook']=fields(line)
            elif line.startswith('IO_ERROR '):current['io_error']=fields(line)
            elif line.startswith('END '):
                current['end']=fields(line);current['complete']=True
                total=current['end']['ticks'];stages=current['stages']
                current['read_percent']=100*stages.get('read',{}).get('raw_ticks',0)/total if total else None
                rtc=current.get('time',{}).get('rtc_elapsed',0)
                current['observed_tick_hz']=total/rtc if rtc else None
                current['tokens_per_second']=current['end']['tokens']/rtc if rtc else None
                current['consistency']={
                    'stage_ticks_match':abs(sum(v['raw_ticks'] for v in stages.values())-total)<=2,
                    'phase_ticks_match':all(sum(current['phases'][p].get(s,{}).get('raw_ticks',0) for p in ('prefill','decode'))==v['raw_ticks'] for s,v in stages.items()),
                    'phase_bytes_match':sum(current['phases'][p].get('bytes',0) for p in ('prefill','decode'))==current.get('perf',{}).get('bytes',-1),
                    'read_bytes_accounted':0<=current.get('io',{}).get('bytes',-1)<=current.get('perf',{}).get('bytes',-2) and (current['end']['state']!=4 or current['io']['bytes']==current['perf']['bytes']),
                    'token_count_match':len(current['tokens'])==current['end']['tokens'],
                    'timing_split_match':sum(current.get('time',{}).get(k,0) for k in ('prefill_ticks','decode_ticks'))==total}
                current=None
    result['exit_recorded']='EXIT' in text.splitlines()
    return result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log',type=Path);parser.add_argument('--output',type=Path)
    args=parser.parse_args();report=analyze(args.log.read_text(encoding='utf-8'))
    blob=json.dumps(report,ensure_ascii=False,indent=2)+'\n'
    if args.output:args.output.write_text(blob,encoding='utf-8')
    print(blob)
if __name__=='__main__':main()
