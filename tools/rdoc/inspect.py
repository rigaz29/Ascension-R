# Read a RenderDoc capture (rdoc.ps1 inspect <params>). Parameters: "capture", "out", "what" (a
# list of the steps below), and the steps' own. Results go to <out>, the log to <out>/inspect.log.
#   actions   every action; draws with their targets, depth, viewport, pixel shader and the
#             pixel-stage textures (slow on a big capture: each draw is replayed). -> actions.txt,
#             textures.txt
#   save      "save": [[event, "resource id", "name"], ...] -> <name>.png as of that event
#   raw       "raw": [[event, "resource id", "name"], ...] -> <name>.bin (mip 0, every slice)
#   swap      the swapchain images as of the last event -> swap_<n>.png
#   pixhist   "points": [[x, y], ...], "textures": ["id", ...] -> pixhist.txt: each event that
#             touched the pixel, passed or not, the shader output, the value after, and for a
#             passing draw its pixel shader and textures
#   event     "event", optional "pixel": [x, y] -> event_<eid>.txt (pipeline name, pixel-stage
#             views with format and swizzle, pixel/vertex disassembly in event_<eid>_ps/_vs.txt,
#             and with "pixel" a debug trace: every variable change, step by step)
#   pick      "event", "uv": [u, v], "textures": ["id", ...] -> pick.txt: mip-0 texel at the uv
#             and the mean of its 3x3 neighbours
#   postvs    "event", "count" -> postvs_<eid>.txt: the vertex shader's output signature and the
#             first <count> vertices' outputs as floats
#   rpcs3     "event" (an RPCS3 capture): rpcs3_<eid>.txt: the draw's fragment constants
#             fc[payload.x + k] and its texture parameters [payload.z + unit] (scale, bias,
#             clamp, remap, flags), payload being the first vertex's draw_params_payload
#   port      "event" (a port capture): port_<eid>.txt: the PSConstants cbuffer as vec4s
import os, json, struct, time, traceback
P = json.load(open(os.environ["RDOC_PARAMS"], encoding="utf-8-sig"))
OUT = P["out"]
os.makedirs(OUT, exist_ok=True)
LOG = os.path.join(OUT, "inspect.log")


def log(s):
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(time.strftime("%H:%M:%S ") + s + "\n")


def ok(res):
    return getattr(res, "code", res) == rd.ResultCode.Succeeded


def rid_s(rid):
    return str(rid).replace("ResourceId::", "")


try:
    import renderdoc as rd
    cap = rd.OpenCaptureFile()
    if not ok(cap.OpenFile(P["capture"], "", None)):
        raise RuntimeError("cannot open " + P["capture"])
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    if not ok(res):
        raise RuntimeError("cannot replay " + P["capture"])
    sf = ctl.GetStructuredFile()
    texs = {rid_s(t.resourceId): t for t in ctl.GetTextures()}
    names = {rid_s(r.resourceId): r.name for r in ctl.GetResources()}
    acts = {}
    order = []

    def walk(al, depth):
        for a in al:
            acts[a.eventId] = a
            order.append((a, depth))
            if len(a.children):
                walk(a.children, depth + 1)
    walk(ctl.GetRootActions(), 0)
    last_event = max(acts) if acts else 0

    def tdesc(rid):
        k = rid_s(rid)
        t = texs.get(k)
        if t is None:
            return k
        nm = names.get(k, "")
        return "%s:%dx%d%s:%s%s" % (k, t.width, t.height, ("x%d" % t.depth) if t.depth > 1 else "",
                                    t.format.Name(), (" '" + nm + "'") if nm and not nm.startswith(("2D ", "3D ")) else "")

    def find_tex(s):
        return texs.get(str(s))

    def views(st, stage):
        out = []
        for u in st.GetReadOnlyResources(stage, True):
            r = u.descriptor.resource
            if r == rd.ResourceId.Null():
                continue
            sw = getattr(u.descriptor, "swizzle", None)
            sws = ""
            if sw is not None:
                try:
                    sws = " swizzle=" + "".join(str(c).split(".")[-1][:1] for c in (sw.red, sw.green, sw.blue, sw.alpha))
                except Exception:
                    pass
            try:
                vf = u.descriptor.format.Name()
            except Exception:
                vf = "?"
            out.append((u.access.index, tdesc(r), vf, sws))
        return out

    def outputs(st):
        o = []
        for d in st.GetOutputTargets():
            r = getattr(d, "resource", None)
            if r is not None and r != rd.ResourceId.Null():
                o.append(tdesc(r))
        return o

    what = P.get("what", [])

    if "actions" in what:
        with open(os.path.join(OUT, "textures.txt"), "w", encoding="utf-8") as f:
            for k, t in texs.items():
                f.write("%s %dx%dx%d mips=%d arr=%d fmt=%s ms=%d flags=%s name=%s\n" % (
                    k, t.width, t.height, t.depth, t.mips, t.arraysize, t.format.Name(), t.msSamp,
                    str(t.creationFlags), names.get(k, "")))
        with open(os.path.join(OUT, "actions.txt"), "w", encoding="utf-8") as f:
            for a, depth in order:
                line = "%s%d %s" % ("  " * depth, a.eventId, a.GetName(sf))
                if a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch):
                    try:
                        ctl.SetFrameEvent(a.eventId, False)
                        st = ctl.GetPipelineState()
                        dd = st.GetDepthTarget()
                        z = getattr(dd, "resource", None)
                        vp = st.GetViewport(0)
                        line += " | n=%d out=[%s] z=%s vp=%dx%d+%d+%d ps=%s tex=[%s]" % (
                            a.numIndices, ",".join(outputs(st)),
                            tdesc(z) if z is not None and z != rd.ResourceId.Null() else "",
                            vp.width, vp.height, vp.x, vp.y, rid_s(st.GetShader(rd.ShaderStage.Pixel)),
                            " ".join("b%d:%s" % (i, d) for i, d, _, _ in views(st, rd.ShaderStage.Pixel)))
                    except Exception as e:
                        line += " | err %s" % e
                elif a.flags & (rd.ActionFlags.Clear | rd.ActionFlags.Copy | rd.ActionFlags.Resolve):
                    line += " | copy %s -> %s" % (tdesc(a.copySource), tdesc(a.copyDestination))
                f.write(line + "\n")
        log("actions: %d, textures: %d" % (len(order), len(texs)))

    for eid, tid, nm in P.get("save", []):
        t = find_tex(tid)
        if t is None:
            log("save: no texture %s" % tid)
            continue
        ctl.SetFrameEvent(int(eid), True)
        ts = rd.TextureSave()
        ts.resourceId = t.resourceId
        ts.alpha = rd.AlphaMapping.Preserve
        ts.destType = rd.FileType.PNG
        ctl.SaveTexture(ts, os.path.join(OUT, nm + ".png"))
        log("saved %s at %s as %s.png" % (tid, eid, nm))

    for eid, tid, nm in P.get("raw", []):
        t = find_tex(tid)
        if t is None:
            log("raw: no texture %s" % tid)
            continue
        ctl.SetFrameEvent(int(eid), True)
        data = ctl.GetTextureData(t.resourceId, rd.Subresource(0, 0, 0))
        open(os.path.join(OUT, nm + ".bin"), "wb").write(bytes(data))
        log("raw %s at %s: %d bytes as %s.bin" % (tid, eid, len(data), nm))

    if "swap" in what and acts:
        ctl.SetFrameEvent(last_event, True)
        n = 0
        for k, t in texs.items():
            if t.creationFlags & rd.TextureCategory.SwapBuffer:
                ts = rd.TextureSave()
                ts.resourceId = t.resourceId
                ts.alpha = rd.AlphaMapping.Discard
                ts.destType = rd.FileType.PNG
                ctl.SaveTexture(ts, os.path.join(OUT, "swap_%d.png" % n))
                n += 1
        log("swap: %d images at event %d" % (n, last_event))

    if "pixhist" in what:
        def col(v):
            c = v.col.floatValue
            return "(%.3f %.3f %.3f %.3f)" % (c[0], c[1], c[2], c[3])
        state = {}
        with open(os.path.join(OUT, "pixhist.txt"), "w", encoding="utf-8") as f:
            for tid in P["textures"]:
                t = find_tex(tid)
                if t is None:
                    f.write("texture %s not found\n" % tid)
                    continue
                for x, y in P["points"]:
                    f.write("== %s at (%d,%d)\n" % (tdesc(t.resourceId), x, y))
                    for m in ctl.PixelHistory(t.resourceId, int(x), int(y), rd.Subresource(0, 0, 0), rd.CompType.Typeless):
                        a = acts.get(m.eventId)
                        flags = [fl for fl in ("depthTestFailed", "stencilTestFailed", "shaderDiscarded", "backfaceCulled",
                                               "scissorClipped", "viewClipped", "depthClipped", "sampleMasked",
                                               "depthBoundsFailed", "predicationSkipped", "directShaderWrite")
                                 if getattr(m, fl, False)]
                        f.write("  e%-6d %-40s %s pre=%s out=%s post=%s %s\n" % (
                            m.eventId, (a.GetName(sf) if a else "?")[:40], "PASS" if m.Passed() else "fail",
                            col(m.preMod), col(m.shaderOut), col(m.postMod), ",".join(flags)))
                        if m.Passed():
                            if m.eventId not in state:
                                try:
                                    ctl.SetFrameEvent(m.eventId, False)
                                    st = ctl.GetPipelineState()
                                    state[m.eventId] = "pipeline '%s' ps=%s tex=[%s]" % (
                                        names.get(rid_s(st.GetGraphicsPipelineObject()), ""),
                                        rid_s(st.GetShader(rd.ShaderStage.Pixel)),
                                        " ".join("b%d:%s" % (i, d) for i, d, _, _ in views(st, rd.ShaderStage.Pixel)))
                                except Exception as e:
                                    state[m.eventId] = "state: %s" % e
                            f.write("           %s\n" % state[m.eventId])
        log("pixhist done")

    if "event" in what:
        eid = int(P["event"])
        ctl.SetFrameEvent(eid, True)
        st = ctl.GetPipelineState()
        pipe = st.GetGraphicsPipelineObject()
        with open(os.path.join(OUT, "event_%d.txt" % eid), "w", encoding="utf-8") as f:
            f.write("event %d pipeline %s '%s'\n" % (eid, rid_s(pipe), names.get(rid_s(pipe), "")))
            f.write("targets %s\n" % ", ".join(outputs(st)))
            for i, d, vf, sws in views(st, rd.ShaderStage.Pixel):
                f.write("  view %d: %s viewfmt=%s%s\n" % (i, d, vf, sws))
            targets = ctl.GetDisassemblyTargets(True)
            for stage, suffix in ((rd.ShaderStage.Pixel, "ps"), (rd.ShaderStage.Vertex, "vs")):
                refl = st.GetShaderReflection(stage)
                if refl is not None and targets:
                    open(os.path.join(OUT, "event_%d_%s.txt" % (eid, suffix)), "w", encoding="utf-8").write(
                        ctl.DisassembleShader(pipe, refl, targets[0]))
            if "pixel" in P:
                x, y = P["pixel"]
                trace = ctl.DebugPixel(int(x), int(y), rd.DebugPixelInputs())
                if trace is None or trace.debugger is None:
                    f.write("no debug trace for (%d,%d)\n" % (x, y))
                else:
                    def var(v):
                        try:
                            vals = list(v.value.f32v)[:max(1, min(16, v.rows * v.columns))]
                            return "%s=(%s)" % (v.name, " ".join("%.4g" % z for z in vals))
                        except Exception:
                            return "%s=?" % getattr(v, "name", "?")
                    f.write("== debug (%d,%d)\n" % (x, y))
                    for v in trace.inputs:
                        f.write("  in %s\n" % var(v))
                    steps = 0
                    while steps < int(P.get("maxsteps", 20000)):
                        states = ctl.ContinueDebug(trace.debugger)
                        if not states:
                            break
                        for s in states:
                            steps += 1
                            f.write("  #%d i%d %s\n" % (s.stepIndex, s.nextInstruction,
                                                        " ".join(var(c.after) for c in s.changes)))
                    if hasattr(ctl, "FreeTrace"):
                        ctl.FreeTrace(trace)
        log("event %d done" % eid)

    if "pick" in what:
        ctl.SetFrameEvent(int(P["event"]), True)
        u, v = P["uv"]
        with open(os.path.join(OUT, "pick.txt"), "a", encoding="utf-8") as f:
            for tid in P["textures"]:
                t = find_tex(tid)
                if t is None:
                    f.write("%s not found\n" % tid)
                    continue
                x = min(t.width - 1, int(u * t.width)); y = min(t.height - 1, int(v * t.height))
                vals = []
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        pv = ctl.PickPixel(t.resourceId, min(t.width - 1, max(0, x + dx)),
                                           min(t.height - 1, max(0, y + dy)), rd.Subresource(0, 0, 0),
                                           rd.CompType.Typeless)
                        vals.append(list(pv.floatValue)[:4])
                c = vals[4]
                m = [sum(w[i] for w in vals) / 9.0 for i in range(4)]
                f.write("%s at (%d,%d): (%.3f %.3f %.3f %.3f) mean3x3 (%.3f %.3f %.3f %.3f)\n" % (
                    tdesc(t.resourceId), x, y, c[0], c[1], c[2], c[3], m[0], m[1], m[2], m[3]))
        log("pick done")

    if "postvs" in what or "rpcs3" in what:
        eid = int(P["event"])
        ctl.SetFrameEvent(eid, True)
        st = ctl.GetPipelineState()
        mf = ctl.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
        stride = mf.vertexByteStride
        if "postvs" in what:
            refl = st.GetShaderReflection(rd.ShaderStage.Vertex)
            data = ctl.GetBufferData(mf.vertexResourceId, mf.vertexByteOffset, int(P.get("count", 6)) * stride)
            with open(os.path.join(OUT, "postvs_%d.txt" % eid), "w", encoding="utf-8") as f:
                f.write("stride %d indices %d\n" % (stride, mf.numIndices))
                for i, s in enumerate(refl.outputSignature):
                    f.write("out %d: %s%d %s comp=%d reg=%d\n" % (i, s.semanticName, s.semanticIndex, s.varName,
                                                                   s.compCount, s.regIndex))
                for v in range(len(data) // stride):
                    fl = struct.unpack_from("<%df" % (stride // 4), data, v * stride)
                    f.write("v%d: %s\n" % (v, " ".join("%.4g" % z for z in fl)))
        if "rpcs3" in what:
            refl = st.GetShaderReflection(rd.ShaderStage.Pixel)
            first = ctl.GetBufferData(mf.vertexResourceId, mf.vertexByteOffset, stride)
            payload = struct.unpack_from("<%dI" % (stride // 4), first, 0)[-4:]
            with open(os.path.join(OUT, "rpcs3_%d.txt" % eid), "w", encoding="utf-8") as f:
                f.write("draw_params_payload %r\n" % (payload,))
                for i, cb in enumerate(refl.constantBlocks):
                    d = st.GetConstantBlock(rd.ShaderStage.Pixel, i, 0).descriptor
                    if "FragmentConstants" in cb.name:
                        raw = ctl.GetBufferData(d.resource, d.byteOffset + payload[0] * 16, 16 * int(P.get("count", 64)))
                        for k in range(len(raw) // 16):
                            f.write("fc %3d: %s\n" % (k, " ".join("%.6g" % z for z in struct.unpack_from("<4f", raw, k * 16))))
                    elif "TextureParameters" in cb.name:
                        raw = ctl.GetBufferData(d.resource, d.byteOffset + payload[2] * 48, 48 * 16)
                        for u in range(len(raw) // 48):
                            f.write("tex %2d scale=(%.4g %.4g %.4g) bias=(%.4g %.4g %.4g) clamp=(%.4g %.4g %.4g %.4g) "
                                    "remap=0x%08X flags=0x%08X\n" % ((u,) + struct.unpack_from("<10f2I", raw, u * 48)))
        log("vertex data done")

    if "shaders" in what:
        # Every distinct pixel shader of the frame, disassembled once (at its first draw):
        # shaders/ps_<id>.txt, and shaders/index.txt naming the event and pipeline of each.
        sd = os.path.join(OUT, "shaders")
        os.makedirs(sd, exist_ok=True)
        targets = ctl.GetDisassemblyTargets(True)
        seen = {}
        with open(os.path.join(sd, "index.txt"), "w", encoding="utf-8") as f:
            for a, _ in order:
                if not (a.flags & rd.ActionFlags.Drawcall):
                    continue
                ctl.SetFrameEvent(a.eventId, False)
                st = ctl.GetPipelineState()
                ps = rid_s(st.GetShader(rd.ShaderStage.Pixel))
                if ps in seen or ps == "0":
                    continue
                seen[ps] = a.eventId
                refl = st.GetShaderReflection(rd.ShaderStage.Pixel)
                pipe = st.GetGraphicsPipelineObject()
                if refl is not None and targets:
                    open(os.path.join(sd, "ps_%s.txt" % ps), "w", encoding="utf-8").write(
                        ctl.DisassembleShader(pipe, refl, targets[0]))
                f.write("ps %s first event %d pipeline '%s'\n" % (ps, a.eventId, names.get(rid_s(pipe), "")))
        log("shaders: %d" % len(seen))

    if "port" in what:
        eid = int(P["event"])
        ctl.SetFrameEvent(eid, True)
        st = ctl.GetPipelineState()
        refl = st.GetShaderReflection(rd.ShaderStage.Pixel)
        with open(os.path.join(OUT, "port_%d.txt" % eid), "w", encoding="utf-8") as f:
            for i, cb in enumerate(refl.constantBlocks):
                d = st.GetConstantBlock(rd.ShaderStage.Pixel, i, 0).descriptor
                f.write("cbuffer %s: %s\n" % (cb.name, [(v.name, v.byteOffset) for v in cb.variables]))
                raw = ctl.GetBufferData(d.resource, d.byteOffset, d.byteSize)
                for k in range(len(raw) // 16):
                    f.write("%3d: %s\n" % (k, " ".join("%.6g" % z for z in struct.unpack_from("<4f", raw, k * 16))))
        log("port constants done")

    ctl.Shutdown()
    cap.Shutdown()
    log("done")
except Exception:
    log("error " + traceback.format_exc())
finally:
    os._exit(0)
