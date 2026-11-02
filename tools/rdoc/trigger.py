# Capture frames of a running process that has RenderDoc loaded (rdoc.ps1 trigger <params>).
# Parameters: "out", "target" (part of the process name, e.g. "rpcs3" or the port's exe name),
# "frames" (default 1), "wait" (seconds to wait for the captures, default 300).
# Logs each new capture's path to <out>/trigger.log.
import os, json, time, traceback
P = json.load(open(os.environ["RDOC_PARAMS"], encoding="utf-8-sig"))
LOG = os.path.join(P["out"], "trigger.log")


def log(s):
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(time.strftime("%H:%M:%S ") + s + "\n")


try:
    import renderdoc as rd
    want = P.get("target", "").lower()
    frames = int(P.get("frames", 1))
    targets = []
    t_end = time.time() + 20
    while not targets and time.time() < t_end:
        ident = 0
        while True:
            ident = rd.EnumerateRemoteTargets("", ident)
            if ident == 0:
                break
            targets.append(ident)
        if not targets:
            time.sleep(1)
    tc = None
    for ident in targets:
        t = rd.CreateTargetControl("", ident, "rdoc-trigger", True)
        if t is None:
            continue
        log("target %s pid %d" % (t.GetTarget(), t.GetPID()))
        if tc is None and want in t.GetTarget().lower():
            tc = t
        else:
            t.Shutdown()
    if tc is None:
        log("no target named like '%s'" % want)
    else:
        t_end = time.time() + 3
        while time.time() < t_end:
            tc.ReceiveMessage(None)
            time.sleep(0.05)
        tc.TriggerCapture(frames)
        got = 0
        t_end = time.time() + float(P.get("wait", 300))
        while got < frames and time.time() < t_end:
            m = tc.ReceiveMessage(None)
            if m.type == rd.TargetControlMessageType.NewCapture:
                c = m.newCapture
                log("capture frame %s bytes %s path %s" % (c.frameNumber, c.byteSize, c.path))
                got += 1
            elif m.type == rd.TargetControlMessageType.Disconnected:
                log("disconnected")
                break
            time.sleep(0.05)
        tc.Shutdown()
        log("done, %d captures" % got)
except Exception:
    log("error " + traceback.format_exc())
finally:
    os._exit(0)
