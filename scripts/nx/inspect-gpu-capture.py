"""Run with portable qrenderdoc --python; outputs stay in ignored build/.

BO3_RDC_FILE and BO3_RDC_OUTPUT select the capture and output directory.
The embedded RenderDoc Python runtime is required, not system Python.
"""
import json
import os
import traceback
import time
from pathlib import Path
import renderdoc as rd

out = Path(os.environ.get('BO3_RDC_OUTPUT', 'C:/bo3recomp/build/draw-capture/inspection'))
out.mkdir(parents=True, exist_ok=True)
cap = None
controller = None

def write(name, data):
    (out / name).write_text(json.dumps(data, indent=2) + '\n')

def progress(value):
    (out / 'progress.txt').write_text(str(value))

def simple(obj, fields):
    result = {}
    for field in fields:
        value = getattr(obj, field)
        result[field] = value if isinstance(value, (int, float, bool, str)) else str(value)
    return result

try:
    filename = os.environ.get('BO3_RDC_FILE', 'C:/bo3recomp/build/draw-capture/white-squares_frame3002.rdc')
    cap = rd.OpenCaptureFile()
    result = cap.OpenFile(filename, '', None)
    progress('OpenFile: ' + str(result))
    if result.code != rd.ResultCode.Succeeded:
        raise RuntimeError(str(result))
    thumbnail = cap.GetThumbnail(rd.FileType.PNG, 0)
    (out / 'thumbnail.png').write_bytes(bytes(thumbnail.data))
    result, controller = cap.OpenCapture(rd.ReplayOptions(), progress)
    if result.code != rd.ResultCode.Succeeded:
        raise RuntimeError(str(result))
    progress('Replay opened')
    structured = controller.GetStructuredFile()
    actions = []
    objects = []
    def visit(items, parents):
        for a in items:
            name = a.GetName(structured)
            row = simple(a, ['eventId', 'actionId', 'flags', 'numIndices', 'numInstances', 'copySource', 'copyDestination', 'depthOut'])
            row.update(name=name, parents=parents, outputs=[str(x) for x in a.outputs])
            actions.append(row)
            objects.append(a)
            visit(a.children, parents + [name])
    visit(controller.GetRootActions(), [])
    write('actions.json', actions)
    resources = {str(r.resourceId): r.name for r in controller.GetResources()}
    write('resources.json', resources)
    textures = controller.GetTextures()
    write('textures.json', [dict(simple(t, ['resourceId', 'width', 'height', 'depth', 'mips', 'arraysize', 'msSamp', 'creationFlags']), name=resources.get(str(t.resourceId)), format=t.format.Name()) for t in textures])
    write('buffers.json', [dict(simple(b, ['resourceId', 'length', 'creationFlags']), name=resources.get(str(b.resourceId))) for b in controller.GetBuffers()])
    controller.SetFrameEvent(max(a.eventId for a in objects), True)
    # Export final swapchain images and record their writers before following draws.
    saved = []
    for t in textures:
        if t.creationFlags & rd.TextureCategory.SwapBuffer:
            save = rd.TextureSave()
            save.resourceId = t.resourceId
            save.destType = rd.FileType.PNG
            path = out / ('final-' + str(t.resourceId).replace('::', '-') + '.png')
            status = controller.SaveTexture(save, str(path))
            saved.append(dict(resource=str(t.resourceId), path=str(path), status=str(status), usage=[simple(u, ['eventId', 'usage']) for u in controller.GetUsage(t.resourceId)]))
    write('final-images.json', saved)
    write('api.json', {'API': str(controller.GetAPIProperties().pipelineType), 'draws': sum(bool(a.flags & rd.ActionFlags.Drawcall) for a in objects), 'actions': len(actions)})
    progress('Inventory complete')
    if os.environ.get('BO3_RDC_INTERACTIVE') == '1':
        def snapshot(event):
            controller.SetFrameEvent(event, True)
            pipe = controller.GetPipelineState()
            data = {'event': event, 'pipeline': str(pipe.GetGraphicsPipelineObject()), 'outputs': [simple(d, ['resource', 'format', 'type']) for d in pipe.GetOutputTargets()], 'stages': {}}
            for stage in [rd.ShaderStage.Vertex, rd.ShaderStage.Fragment, rd.ShaderStage.Compute]:
                reflection = pipe.GetShaderReflection(stage)
                if reflection is None:
                    continue
                sid = str(reflection.resourceId)
                shader_path = out / (sid.replace('::', '-') + '.txt')
                if not shader_path.exists():
                    shader_path.write_text(controller.DisassembleShader(pipe.GetGraphicsPipelineObject(), reflection, ''))
                    (out / (sid.replace('::', '-') + '.spv')).write_bytes(bytes(reflection.rawBytes))
                resources_used = []
                for used in pipe.GetReadOnlyResources(stage, True):
                    resources_used.append({'access': simple(used.access, ['index', 'arrayElement', 'byteOffset']), 'descriptor': simple(used.descriptor, ['resource', 'secondary', 'view', 'format', 'swizzle', 'firstMip', 'numMips', 'type', 'byteOffset', 'byteSize'])})
                data['stages'][str(stage)] = {'shader': sid, 'read_only': resources_used, 'read_write': [simple(u.descriptor, ['resource', 'type', 'byteOffset', 'byteSize']) for u in pipe.GetReadWriteResources(stage, True)]}
            data['blends'] = [{'enabled': b.enabled, 'writeMask': b.writeMask, 'colorBlend': simple(b.colorBlend, ['source', 'destination', 'operation']), 'alphaBlend': simple(b.alphaBlend, ['source', 'destination', 'operation'])} for b in pipe.GetColorBlends()]
            write('event-' + str(event) + '.json', data)
        progress('Ready for command.json')
        while True:
            command_path = out / 'command.json'
            if not command_path.exists():
                time.sleep(0.25)
                continue
            command = json.loads(command_path.read_text())
            command_path.unlink()
            try:
                kind = command['kind']
                progress('Executing ' + str(command))
                if kind == 'exit':
                    break
                if kind == 'snapshot':
                    for event in command['events']:
                        snapshot(event)
                if kind == 'textures':
                    controller.SetFrameEvent(command.get('event', max(a.eventId for a in objects)), True)
                    rows = []
                    for number in command['resources']:
                        texture = next(t for t in textures if str(t.resourceId) == 'ResourceId::' + str(number))
                        save = rd.TextureSave()
                        save.resourceId = texture.resourceId
                        save.destType = rd.FileType.PNG
                        save.alpha = rd.AlphaMapping.Preserve
                        filename = 'texture-' + str(number) + '-' + str(command.get('event', 'final')) + '.png'
                        rows.append({'resource': number, 'result': str(controller.SaveTexture(save, str(out / filename))), 'usage': [simple(u, ['eventId', 'usage']) for u in controller.GetUsage(texture.resourceId)]})
                        (out / (filename + '.bin')).write_bytes(bytes(controller.GetTextureData(texture.resourceId, rd.Subresource())))
                    write('texture-usages.json', rows)
                if kind == 'detail':
                    def variable(v):
                        return dict(simple(v, ['name', 'type', 'rows', 'columns']), floats=list(v.value.f32v), uints=list(v.value.u32v), members=[variable(x) for x in v.members])
                    for event in command['events']:
                        controller.SetFrameEvent(event, True)
                        pipe = controller.GetPipelineState()
                        stage = rd.ShaderStage.Compute if command.get('stage') == 'compute' else rd.ShaderStage.Fragment
                        reflection = pipe.GetShaderReflection(stage)
                        data = {'event': event, 'viewport': simple(pipe.GetViewport(0), ['x','y','width','height','minDepth','maxDepth']), 'constants': [], 'samplers': []}
                        for used in pipe.GetConstantBlocks(stage, True):
                            slot = used.access.index
                            d = used.descriptor
                            variables = controller.GetCBufferVariableContents(pipe.GetGraphicsPipelineObject(), reflection.resourceId, stage, reflection.entryPoint, slot, d.resource, d.byteOffset, d.byteSize)
                            data['constants'].append({'name': reflection.constantBlocks[slot].name, 'variables': [variable(v) for v in variables]})
                        for used in pipe.GetSamplers(stage, True):
                            data['samplers'].append({'access': simple(used.access, ['index', 'arrayElement']), 'sampler': simple(used.sampler, ['object','addressU','addressV','addressW','minLOD','maxLOD','mipBias','unnormalized']), 'filter': simple(used.sampler.filter, ['minify','magnify','mip'])})
                        write('detail-' + str(event) + '.json', data)
                        if command.get('edram'):
                            buffer = next(b for b in controller.GetBuffers() if str(b.resourceId) == 'ResourceId::268')
                            (out / ('edram-' + str(event) + '.bin')).write_bytes(bytes(controller.GetBufferData(buffer.resourceId, 0, buffer.length)))
                progress('Command complete')
            except Exception:
                (out / 'command-error.txt').write_text(traceback.format_exc())
                progress('Command failed')
except BaseException:
    (out / 'error.txt').write_text(traceback.format_exc())
finally:
    if controller is not None:
        controller.Shutdown()
    if cap is not None:
        cap.Shutdown()
raise SystemExit()
