#!/usr/bin/env python3
"""Read Kingston SATA SMART temperature and feed YogaSMC's read-only TH0A cache."""
import argparse, datetime, json, pathlib, plistlib, subprocess, time
parser=argparse.ArgumentParser();parser.add_argument('--once',action='store_true');args=parser.parse_args()
base=pathlib.Path(__file__).resolve().parent

def sample():
    record={'timestamp':datetime.datetime.now(datetime.timezone.utc).isoformat()}
    try:
        p=subprocess.run([str(base/'smartctl'),'-A','-i','-j','/dev/disk0'],capture_output=True,text=True,timeout=8)
        data=json.loads(p.stdout)
        if p.returncode & 7:raise RuntimeError('smartctl acquisition failed: '+str(p.returncode))
        model=data.get('model_name','');record['model']=model;record['serial']=data.get('serial_number')
        if model!='KINGSTON RBUSNS8180DS3256GJ':raise RuntimeError('Unexpected disk0 identity: '+model)
        temp=data.get('temperature',{}).get('current')
        if not isinstance(temp,(int,float)) or not 0<=temp<=125:raise RuntimeError('Invalid/missing SMART temperature')
        record['celsius']=temp
        r=subprocess.run(['/usr/sbin/ioreg','-a','-r','-c','DYSMC'],capture_output=True,timeout=5,check=True)
        nodes=plistlib.loads(r.stdout)
        if not nodes or nodes[0].get('HPThermalSMCVersion')!=1:
            record['status']='waiting-for-new-kext';return record
        p=subprocess.run([str(base/'ioset'),'DYSMC','SSDTemperatureDeciC',str(round(temp*10))],capture_output=True,text=True,timeout=5)
        record['status']='published' if p.returncode==0 else 'publish-error';record['result']=p.stdout.strip()
    except Exception as e:record['status']='unavailable';record['error']=str(e)
    return record
while True:
    print(json.dumps(sample()),flush=True)
    if args.once:break
    time.sleep(10)
