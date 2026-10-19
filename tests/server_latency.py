"""Single-client request/response latency probe; informational, not a timing assertion."""
import argparse
import json
import os
import statistics
import subprocess
import time
from wire import Peer

parser=argparse.ArgumentParser()
parser.add_argument('--server',required=True)
parser.add_argument('--iterations',type=int,default=100)
args=parser.parse_args()
env=dict(os.environ,CHAT_CREDENTIALS='bench:user:secret')
process=subprocess.Popen([args.server,'--memory','--port','0','--threads','2'],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
try:
    ready=process.stdout.readline().strip()
    assert ready.startswith('READY '),ready
    with Peer(int(ready.rsplit(':',1)[1])) as peer:
        peer.hello('bench','user','secret')
        for i in range(10):
            request=peer.send(9,[]);peer.wait(10,request)
        samples=[]
        for i in range(args.iterations):
            start=time.perf_counter_ns()
            request=peer.send(9,[]);peer.wait(10,request)
            samples.append((time.perf_counter_ns()-start)/1e6)
    samples.sort()
    print(json.dumps({'iterations':len(samples),'median_ms':statistics.median(samples),'p95_ms':samples[min(len(samples)-1,int(len(samples)*.95))],'max_ms':max(samples)}))
finally:
    process.terminate()
    try:process.communicate(timeout=15)
    except subprocess.TimeoutExpired:
        process.kill();process.communicate()
