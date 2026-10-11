import importlib.util, sys, statistics, collections, os
sys.argv=['x','probe','0']
spec=importlib.util.spec_from_file_location('sb',os.path.join(os.path.dirname(os.path.abspath(__file__)), 'scrollbench.py')); sb=importlib.util.module_from_spec(spec); spec.loader.exec_module(sb)
dur=collections.defaultdict(list)
for _ in range(2):
    ev,_=sb.trace_scroll()
    open_={}
    for e in sorted(ev, key=lambda e: e['ts']):
        if e.get('ph') in ('b','e') and e.get('cat','').find('benchmark')>=0:
            k=(e['name'], e.get('pid'), str(e.get('id2') or e.get('id')))
            if e['ph']=='b': open_[k]=e['ts']
            elif k in open_: dur[(e['name'], e['pid'])].append((e['ts']-open_.pop(k))/1000)
        elif e.get('ph')=='X' and e.get('dur') and e['name'] in ('Display::DrawAndSwap','SkiaOutputSurfaceImplOnGpu::SwapBuffers','GLRenderer::DrawFrame','SkiaRenderer::DrawFrame','DirectRenderer::DrawFrame','Graphics.Pipeline.DrawAndSwap','LayerTreeHostImpl::PrepareToDraw','ProxyMain::BeginMainFrame','ThreadProxy::BeginMainFrame','LocalFrameView::RunPaintLifecyclePhase','RasterTask'):
            dur[(e['name'], e['pid'])].append(e['dur']/1000)
rows=[(statistics.mean(v), k, len(v), statistics.median(v), max(v)) for k,v in dur.items() if len(v)>5]
for m,k,n,med,mx in sorted(rows, reverse=True)[:22]: print(f'{m:7.1f} avg {med:7.1f} med {mx:7.1f} max  n={n:4d}  {k[0]} (pid {k[1]})')
