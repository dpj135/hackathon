"""Construct high-iov probes; keep expanded JSON out of the Git repository."""
import json
from pathlib import Path

def generate(code: Path, family: str, output: Path):
    inst=json.loads((code/'data/small_aligned.json').read_text())['instance']
    res=inst['resources']
    if 'mooncake_capacity' in res:res['kvcache_pool_capacity']=res.pop('mooncake_capacity')
    count=32768
    def event(ident,when,ios):
        return dict(event='IO_SUBMIT',time_us=when,submit=dict(submit_id=ident,arrival_us=when,iocbs=ios))
    def io(ident,card,offset,addr,fragmented):
        return dict(iocb_id=ident,opcode='READV',device_id=card,ssd_offset=offset,length=count,
                    iovs=[dict(iov_id=j,hbm_addr=addr+j*2,length=1) for j in range(count)] if fragmented else [dict(iov_id=0,hbm_addr=addr,length=count)])
    if family=='micro':
        events=[event(1,0,[io(i,0,i*count,2**36+i*2**26,True) for i in range(6)])]
    elif family=='filtered':
        events=[event(1,0,[io(i,i%8,(i%8)*2**20,2**36+i*2**24,False) for i in range(48)]),
                event(2,1000,[io(i,i%8,(i%8)*2**20 if i%8 else 2**28,2**36+i*2**24,True) for i in range(48)])]
    else:raise ValueError(family)
    output.write_text(json.dumps(dict(instance=inst,events=events),separators=(',',':')))
