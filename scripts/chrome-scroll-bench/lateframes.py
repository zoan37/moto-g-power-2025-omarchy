# For late presentation gaps, which pipeline stage of the preceding frame was long?
import importlib.util, sys, statistics, collections, os
sys.argv=['x','probe','0']
spec=importlib.util.spec_from_file_location('sb', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'scrollbench.py')); sb=importlib.util.module_from_spec(spec); spec.loader.exec_module(sb)
STAGES=['SendBeginMainFrameToCommit','Commit','EndCommitToActivation','Activation','EndActivateToSubmitCompositorFrame','SubmitToReceiveCompositorFrame','ReceiveCompositorFrameToStartDraw','StartDrawToSwapStart','Swap','SwapEndToPresentationCompositorFrame','RendererCompositorQueueingDelay','BeginImplFrameToSendBeginMainFrame']
agg={'ok':collections.defaultdict(list),'late':collections.defaultdict(list)}
for run in range(4):
    ev,_=sb.trace_scroll()
    swaps,gaps,states=sb.analyze(ev)
    # spans by trace id of PipelineReporter children: use nesting by time within renderer pid
    sub=[e for e in ev if e.get('name')=='SubmitCompositorFrameToPresentationCompositorFrame' and e.get('ph')=='e']
    if not sub: continue
    pid=max(set(e['pid'] for e in sub), key=lambda p: sum(1 for e in sub if e['pid']==p))
    spans=collections.defaultdict(dict)
    for e in ev:
        if e.get('pid')!=pid or e.get('ph') not in ('b','e') or e['name'] not in STAGES+['PipelineReporter']: continue
        k=(e['name'], str(e.get('id2') or e.get('id')))
        spans[k][e['ph']]=e['ts']
    reps=sorted((v['b'],v['e']) for (n,i),v in spans.items() if n=='PipelineReporter' and 'b' in v and 'e' in v)
    st=[(n,v['b'],v['e']) for (n,i),v in spans.items() if n in STAGES and 'b' in v and 'e' in v]
    pres=sorted(set(e['ts'] for e in sub if e['pid']==pid))
    for a,b in zip(pres,pres[1:]):
        g=(b-a)/1000
        if g>100: continue
        kind='late' if g>12.5 else 'ok'
        # stages that ended at this presentation's frame: those with end within [a, b]
        for n,s0,s1 in st:
            if a < s1 <= b: agg[kind][n].append((s1-s0)/1000)
for kind in ('ok','late'):
    print('==',kind)
    for n in STAGES:
        v=agg[kind][n]
        if v: print(f'  {n:42s} n={len(v):4d} med {statistics.median(v):6.1f}  p90 {sorted(v)[int(.9*len(v))]:6.1f}')
