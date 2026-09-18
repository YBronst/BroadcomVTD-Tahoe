#!/usr/bin/env python3
"""Run actual-source host models and public synthetic/static tests; no private captures."""
import json, os, pathlib, re, subprocess, sys
R=pathlib.Path(__file__).resolve().parents[1];O=R/'build/poc-0.2.25'
O.mkdir(parents=True,exist_ok=True)
cc=subprocess.check_output(['xcrun','--find','clang++'],text=True).strip()
sdk=subprocess.check_output(['xcrun','--sdk','macosx','--show-sdk-path'],text=True).strip()
flags=['-std=c++14','-isysroot',sdk,'-pthread']
SANITIZER_FLAGS=['-fsanitize=address,undefined','-fno-sanitize-recover=undefined','-O1','-g']
SANITIZER_ENV={'ASAN_OPTIONS':'halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'}
sanitized_executables=set()
def command_environment(cmd):
    env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1')
    if cmd[0] in sanitized_executables:
        env.update(SANITIZER_ENV)
    return env
cases=['native','native-specific','golden','split','pressure','allocation','prepare','segment-failure','bad-detach',
 'reset-false','unseen','all-ones','zero','generation','owner','ring','stale','halt','mutation',
 'reentry','complete-failure','concurrent','new-generation','wrong-thread','duplicate','newer-active','two-generations',
 'active-cycles','active-requeue']+['active-'+x for x in ['false','unseen','all-ones','zero','halt','blocked',
 'prequarantine','mutation','reentry','complete-failure','new-generation','fence','wrong-thread','owner','ring']]
cases+=['bulk-cycles','bulk-requeue','bulk-failure','bulk-callers','revision-unknown','revision-changed','revision-fatal']
commands=[[sys.executable,'-B','tools/generate_poc_'+x+'_test.py'] for x in ['private','qualification','admission']]
for sanitized in [False,True]:
    out=O/('poc-private-sanitized' if sanitized else 'poc-private-optimized')
    if sanitized: sanitized_executables.add(str(out))
    commands.append([cc]+flags+(SANITIZER_FLAGS if sanitized else ['-O2'])+['tests/poc_private_tx.cpp','-o',str(out)])
    commands.extend([[str(out),c] for c in cases])
models={'tx_quiescence':[''],'tx_disposition':[''],'tx_qualification':[''],
 'tx_admission':['mixed','quarantine','fault','failures'],'read_policy':[''],'tx_status_word':[''],
 'first_failure':['','rolling','already-frozen','concurrent','dropped-trigger'],
 'tx_packet':[''],'tx_cleanup':['','complete-failure'],'rx_cleanup':['','complete-failure'],
 'rx_core':['','complete-failure'],'rx_observation':[''],'mapper_selection':[''],
 'lifetime':[''],'core':['','completion-failure']}
for name,args in models.items():
    out=O/('poc-'+name)
    commands.append([cc]+flags+['-O2','tests/poc_'+name+'.cpp','-o',str(out)])
    commands.extend([[str(out)]+([a] if a else []) for a in args])
for name,args in [('tx_admission',['mixed','quarantine','fault','failures']),('rx_cleanup',['','complete-failure'])]:
    out=O/('poc-'+name+'-sanitized')
    sanitized_executables.add(str(out))
    commands.append([cc]+flags+SANITIZER_FLAGS+['tests/poc_'+name+'.cpp','-o',str(out)])
    commands.extend([[str(out)]+([a] if a else []) for a in args])
commands.append([sys.executable,'-B','-m','unittest','discover','-s','tests','-p','test_*.py'])
results=[];python_tests=0
with (O/'host-tests.log').open('w') as log:
    for cmd in commands:
        run=subprocess.run(cmd,cwd=R,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                           env=command_environment(cmd))
        relative=[x.replace(str(R)+'/','') for x in cmd]
        log.write('COMMAND '+repr(relative)+'\n'+run.stdout);log.flush()
        checks=sum(map(int,re.findall(r': (\d+) checks',run.stdout)))
        results.append(dict(command=relative,returncode=run.returncode,checks=checks))
        if 'unittest' in cmd:
            match=re.search(r'Ran (\d+) tests',run.stdout);python_tests=int(match[1]) if match else 0
        if run.returncode:
            print('FAIL',relative,'see build/poc-0.2.25/host-tests.log');raise SystemExit(run.returncode)
summary=dict(result='PASS',private_cases=len(cases),private_runs=2*len(cases),
 private_checks=sum(x['checks'] for x in results if len(x['command'])==2 and 'poc-private-' in x['command'][0]),
 python_tests=python_tests,commands=len(results),sanitizers='ASan + UBSan',
 sanitizer_flags=SANITIZER_FLAGS,sanitizer_environment=SANITIZER_ENV,
 physical_validation=False,results=results)
(O/'public-test-results.json').write_text(json.dumps(summary,indent=2)+'\n')
print('PASS public suite:',summary['private_runs'],'private runs,',summary['private_checks'],'checks,',python_tests,'Python tests')
