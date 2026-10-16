#!/usr/bin/env python
"""Text report of a RenderDoc capture of the engine (tools/gpu/frame_report.py).

    python tools/gpu/frame_report.py Saved/Captures/map_20261016_120000_frame120.rdc
    python tools/gpu/frame_report.py capture.rdc --json report.json --markdown report.md
    python tools/gpu/frame_report.py --self-test

The report is for people and agents who can not open the RenderDoc UI:
  - the tree of marker groups of the frame (Frame > Scene / Debug Lines / UI / Thumbnail: <asset> ...);
  - draws, dispatches, clears, copies and triangles per group;
  - pipelines and shaders used, render targets (size, format) per group;
  - the API calls of the frame by name (resource barriers and so on) and warnings.

How it reads the capture, in this order:
  1. `import renderdoc` in this interpreter (RenderDoc's python module, when the python version matches; RENDERDOC_DIR
     or the Program Files folder is added to sys.path): the full replay report with render targets and shaders;
  2. `renderdoccmd.exe convert -c xml`: the API calls of the capture in order, with the marker groups rebuilt from
     BeginEvent/EndEvent. Needs no GPU and no python module: draws, triangles, pipelines, clears, copies, API calls.
Set RENDERDOC_DIR if RenderDoc is not in Program Files.

The same markers the engine writes show in the report, and `myengine.exe --frame-stats N` gives the numbers of the
engine's own counters without RenderDoc.
"""

import json
import os
import subprocess
import sys



# --------------------------------------------------------------------------------------------------------------------
# Report model (no RenderDoc here: it is what the formatters and the self test use)
# --------------------------------------------------------------------------------------------------------------------

def new_group(name, depth):
    return {
        "name": name,
        "depth": depth,
        "draws": 0,
        "dispatches": 0,
        "clears": 0,
        "copies": 0,
        "triangles": 0,
        "pipelines": [],
        "shaders": [],
        "renderTargets": [],
        "children": [],
    }


def add_unique(items, value):
    if value and value not in items:
        items.append(value)


def totals(group):
    result = {"draws": group["draws"], "dispatches": group["dispatches"], "clears": group["clears"],
              "copies": group["copies"], "triangles": group["triangles"]}
    for child in group["children"]:
        child_totals = totals(child)
        for key in result:
            result[key] += child_totals[key]
    return result


def flatten(group, out=None):
    if out is None:
        out = []
    out.append(group)
    for child in group["children"]:
        flatten(child, out)
    return out


def make_warnings(report):
    warnings = []
    root = report["root"]
    groups = flatten(root)
    if len(groups) <= 1:
        warnings.append("The frame has no marker groups: the capture was not made with an engine that writes markers.")
    total = totals(root)
    if total["draws"] == 0:
        warnings.append("The frame has no draw calls.")
    thumbs = [g for g in groups if g["name"].startswith("Thumbnail:") or g["name"].startswith("Live view:")]
    if len(thumbs) > 8:
        warnings.append("%d offscreen preview passes in one frame: the thumbnail budget may be too high." % len(thumbs))
    barriers = report["apiCalls"].get("ID3D12GraphicsCommandList::ResourceBarrier", 0)
    if total["draws"] and barriers > total["draws"]:
        warnings.append("More resource barriers (%d) than draws (%d)." % (barriers, total["draws"]))
    for group in groups:
        group_totals = totals(group)
        if group["depth"] >= 1 and group_totals["draws"] > 2000:
            warnings.append("Group '%s' has %d draws: batch or instance them." % (group["name"], group_totals["draws"]))
    if len(report.get("pipelines", [])) > 40:
        warnings.append("%d different pipeline states in one frame." % len(report["pipelines"]))
    return warnings


def format_group_rows(group, rows):
    group_totals = totals(group)
    targets = ", ".join(group["renderTargets"])
    rows.append("| %s%s | %d | %d | %d | %d | %d | %s |" % (
        "&nbsp;&nbsp;" * group["depth"], group["name"], group_totals["draws"], group_totals["dispatches"],
        group_totals["clears"], group_totals["copies"], group_totals["triangles"], targets))
    for child in group["children"]:
        format_group_rows(child, rows)


def format_markdown(report):
    root = report["root"]
    total = totals(root)
    lines = []
    lines.append("# GPU capture report: %s" % os.path.basename(report.get("capture", "")))
    lines.append("")
    lines.append("- API: %s" % report.get("api", "?"))
    lines.append("- events: %s" % report.get("eventCount", "?"))
    lines.append("- draws: %d, dispatches: %d, clears: %d, copies: %d, triangles: %d" % (
        total["draws"], total["dispatches"], total["clears"], total["copies"], total["triangles"]))
    lines.append("- pipelines: %d, shaders: %d" % (len(report.get("pipelines", [])), len(report.get("shaders", []))))
    lines.append("")
    lines.append("| Group | Draws | Dispatches | Clears | Copies | Triangles | Render targets |")
    lines.append("| --- | ---: | ---: | ---: | ---: | ---: | --- |")
    rows = []
    format_group_rows(root, rows)
    lines.extend(rows)
    lines.append("")

    calls = report.get("apiCalls", {})
    if calls:
        lines.append("## API calls of the frame")
        lines.append("")
        lines.append("| Call | Count |")
        lines.append("| --- | ---: |")
        for name, count in sorted(calls.items(), key=lambda item: -item[1])[:25]:
            lines.append("| %s | %d |" % (name, count))
        lines.append("")

    if report.get("shaders"):
        lines.append("## Shaders")
        lines.append("")
        for shader in report["shaders"]:
            lines.append("- %s" % shader)
        lines.append("")

    warnings = report.get("warnings", [])
    lines.append("## Warnings")
    lines.append("")
    if warnings:
        for warning in warnings:
            lines.append("- %s" % warning)
    else:
        lines.append("- none")
    lines.append("")
    return "\n".join(lines)


def to_json(report):
    def convert(group):
        data = dict(group)
        data["totals"] = totals(group)
        data["children"] = [convert(child) for child in group["children"]]
        return data

    result = dict(report)
    result["root"] = convert(report["root"])
    return json.dumps(result, indent=2)


# --------------------------------------------------------------------------------------------------------------------
# RenderDoc side
# --------------------------------------------------------------------------------------------------------------------

def find_renderdoc_dir():
    candidates = []
    if os.environ.get("RENDERDOC_DIR"):
        candidates.append(os.environ["RENDERDOC_DIR"])
    for variable in ("ProgramW6432", "ProgramFiles", "ProgramFiles(x86)"):
        base = os.environ.get(variable)
        if base:
            candidates.append(os.path.join(base, "RenderDoc"))
    for candidate in candidates:
        if os.path.isdir(candidate):
            return candidate
    return None


def flag_value(enum_type, *names):
    """The first name that the enum has: RenderDoc renamed DrawFlags to ActionFlags at 1.30"""
    for name in names:
        if hasattr(enum_type, name):
            return getattr(enum_type, name)
    return 0


def build_report(rd, controller, capture_path):
    """Fills the report model from an open replay controller. `rd` is the renderdoc module."""
    structured = controller.GetStructuredFile()
    flags_type = rd.ActionFlags if hasattr(rd, "ActionFlags") else rd.DrawFlags
    f_draw = flag_value(flags_type, "Drawcall")
    f_dispatch = flag_value(flags_type, "Dispatch")
    f_clear = flag_value(flags_type, "Clear")
    f_copy = flag_value(flags_type, "Copy")
    f_push = flag_value(flags_type, "PushMarker")
    f_pop = flag_value(flags_type, "PopMarker")
    f_set = flag_value(flags_type, "SetMarker")

    if hasattr(controller, "GetRootActions"):
        roots = controller.GetRootActions()
    else:
        roots = controller.GetDrawcalls()

    textures = {}
    for texture in controller.GetTextures():
        textures[texture.resourceId] = texture

    def name_of(action):
        if hasattr(action, "GetName"):
            return action.GetName(structured)
        return getattr(action, "name", "")

    pipelines = []
    shaders = []
    event_count = [0]

    def describe_targets(action):
        controller.SetFrameEvent(action.eventId, False)
        state = controller.GetPipelineState()
        result = []
        try:
            for output in state.GetOutputTargets():
                texture = textures.get(output.resourceId)
                if texture is not None:
                    result.append("%dx%d %s" % (texture.width, texture.height, str(texture.format.Name())))
            depth = state.GetDepthTarget()
            texture = textures.get(depth.resourceId)
            if texture is not None:
                result.append("depth %dx%d" % (texture.width, texture.height))
            for stage_name in ("Vertex", "Pixel", "Compute"):
                stage = getattr(rd.ShaderStage, stage_name, None)
                if stage is None:
                    continue
                reflection = state.GetShaderReflection(stage)
                if reflection is not None:
                    add_unique(shaders, "%s: %s" % (stage_name, str(reflection.debugInfo.entrypoint or state.GetShaderEntryPoint(stage))))
            pipeline = state.GetGraphicsPipelineObject()
            add_unique(pipelines, str(pipeline))
        except Exception as error:  # a pipeline query that this RenderDoc build does not support must not stop the report
            result.append("(state not read: %s)" % error)
        return result

    def walk(action, group):
        event_count[0] += 1
        flags = action.flags
        if flags & f_push:
            child = new_group(name_of(action), group["depth"] + 1)
            group["children"].append(child)
            for sub in action.children:
                walk(sub, child)
            return
        if flags & (f_pop | f_set):
            return
        if flags & f_draw:
            group["draws"] += 1
            instances = max(int(getattr(action, "numInstances", 1)), 1)
            group["triangles"] += (int(getattr(action, "numIndices", 0)) // 3) * instances
            if group["draws"] == 1:
                for target in describe_targets(action):
                    add_unique(group["renderTargets"], target)
        elif flags & f_dispatch:
            group["dispatches"] += 1
        elif flags & f_clear:
            group["clears"] += 1
        elif flags & f_copy:
            group["copies"] += 1
        for sub in action.children:
            walk(sub, group)

    root = new_group("Frame", 0)
    for action in roots:
        walk(action, root)

    # The marker "Frame" of the engine is the root itself when it is the only top-level group
    if len(root["children"]) == 1 and root["children"][0]["name"] == "Frame" and not any(
            root[key] for key in ("draws", "dispatches", "clears", "copies")):
        root = root["children"][0]
        root["depth"] = 0

        def fix_depth(group, depth):
            group["depth"] = depth
            for child in group["children"]:
                fix_depth(child, depth + 1)
        fix_depth(root, 0)

    api_calls = {}
    try:
        for chunk in structured.chunks:
            api_calls[chunk.name] = api_calls.get(chunk.name, 0) + 1
    except Exception:
        pass

    api_name = "?"
    try:
        api_name = str(controller.GetAPIProperties().pipelineType)
    except Exception:
        pass

    report = {
        "capture": capture_path,
        "api": api_name,
        "eventCount": event_count[0],
        "apiCalls": api_calls,
        "pipelines": pipelines,
        "shaders": shaders,
        "root": root,
    }
    report["warnings"] = make_warnings(report)
    return report


def report_from_capture(rd, capture_path):
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    capture = rd.OpenCaptureFile()
    status = capture.OpenFile(capture_path, "", None)
    if status != rd.ResultCode.Succeeded:
        raise RuntimeError("OpenFile failed: %s" % str(status))
    if not capture.LocalReplaySupport():
        raise RuntimeError("This machine can not replay the capture (GPU or API not supported)")
    status, controller = capture.OpenCapture(rd.ReplayOptions(), None)
    if status != rd.ResultCode.Succeeded:
        raise RuntimeError("OpenCapture failed: %s" % str(status))
    try:
        return build_report(rd, controller, capture_path)
    finally:
        controller.Shutdown()
        capture.Shutdown()
        rd.ShutdownReplay()


def write_outputs(report, json_path, markdown_path):
    markdown = format_markdown(report)
    if json_path:
        with open(json_path, "w") as handle:
            handle.write(to_json(report))
    if markdown_path:
        with open(markdown_path, "w") as handle:
            handle.write(markdown)
    return markdown


# --------------------------------------------------------------------------------------------------------------------
# Entry points
# --------------------------------------------------------------------------------------------------------------------

def find_renderdoccmd(renderdoc_dir):
    if renderdoc_dir:
        candidate = os.path.join(renderdoc_dir, "renderdoccmd.exe")
        if os.path.isfile(candidate):
            return candidate
    return None


def child_text(element, name):
    child = element.find("*[@name='%s']" % name)
    return child.text if child is not None and child.text else ""


def int_value(element, name, default=0):
    try:
        return int(float(child_text(element, name)))
    except ValueError:
        return default


def build_report_from_xml(xml_path, capture_path):
    """The report from `renderdoccmd convert -c xml`: the chunks (API calls) of the capture in order, with the marker
    groups rebuilt from BeginEvent/EndEvent. No GPU and no RenderDoc python module are needed."""
    import xml.etree.ElementTree as ET

    root = new_group("Frame", 0)
    stack = [root]
    api_calls = {}
    pipelines = []
    event_count = 0
    topology = ""
    api_name = "?"
    viewport = ""

    for _, element in ET.iterparse(xml_path, events=("end",)):
        if element.tag == "driver":
            api_name = element.text or "?"
            continue
        if element.tag != "chunk":
            continue

        name = element.get("name", "")
        short = name.split("::")[-1]
        if name.startswith("ID3D12GraphicsCommandList::") or name.startswith("IDXGISwapChain::"):
            api_calls[name] = api_calls.get(name, 0) + 1
            event_count += 1
        group = stack[-1]

        if short == "BeginEvent":
            child = new_group(child_text(element, "MarkerText") or "(unnamed)", group["depth"] + 1)
            group["children"].append(child)
            stack.append(child)
        elif short == "EndEvent":
            if len(stack) > 1:
                stack.pop()
        elif short == "IASetPrimitiveTopology":
            node = element.find("*[@name='PrimitiveTopology']")
            topology = node.get("string", "") if node is not None else ""
        elif short == "SetPipelineState":
            pipeline = child_text(element, "pPipelineState")
            if pipeline:
                add_unique(pipelines, "pipeline " + pipeline)
                add_unique(group["pipelines"], "pipeline " + pipeline)
        elif short == "RSSetViewports":
            width = height = 0
            for item in element.iter("float"):
                if item.get("name") == "Width":
                    width = int(float(item.text))
                elif item.get("name") == "Height":
                    height = int(float(item.text))
            viewport = "viewport %dx%d" % (width, height) if width else ""
        elif short in ("DrawIndexedInstanced", "DrawInstanced"):
            group["draws"] += 1
            count = int_value(element, "IndexCountPerInstance" if short == "DrawIndexedInstanced" else "VertexCountPerInstance")
            instances = max(int_value(element, "InstanceCount", 1), 1)
            if topology.endswith("TRIANGLELIST"):
                group["triangles"] += (count // 3) * instances
            elif topology.endswith("TRIANGLESTRIP"):
                group["triangles"] += max(count - 2, 0) * instances
            add_unique(group["renderTargets"], viewport)
        elif short in ("Dispatch", "ExecuteIndirect"):
            group["dispatches"] += 1
        elif short.startswith("Clear"):
            group["clears"] += 1
        elif short.startswith("Copy") or short in ("ResolveSubresource", "ResolveSubresourceRegion"):
            group["copies"] += 1
        element.clear()

    # the engine's own "Frame" marker is the root of the report when it is the only top-level group
    if len(root["children"]) == 1 and not any(root[key] for key in ("draws", "dispatches", "clears", "copies")):
        single = root["children"][0]
        if single["name"] == "Frame":
            root = single

            def fix_depth(group, depth):
                group["depth"] = depth
                for child in group["children"]:
                    fix_depth(child, depth + 1)
            fix_depth(root, 0)

    report = {
        "capture": capture_path,
        "api": api_name,
        "eventCount": event_count,
        "apiCalls": api_calls,
        "pipelines": pipelines,
        "shaders": [],
        "root": root,
        "source": "renderdoccmd convert -c xml (the API calls of the capture; no shader or texture data)",
    }
    report["warnings"] = make_warnings(report)
    return report


def report_from_renderdoccmd(capture_path, renderdoccmd):
    import tempfile

    with tempfile.TemporaryDirectory() as folder:
        xml_path = os.path.join(folder, "capture.xml")
        process = subprocess.run(
            [renderdoccmd, "convert", "-f", os.path.abspath(capture_path), "-c", "xml", "-o", xml_path],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
        if process.returncode != 0 or not os.path.isfile(xml_path):
            raise RuntimeError("renderdoccmd convert failed (%d): %s" % (
                process.returncode, process.stdout.decode("utf-8", "replace").strip()))
        return build_report_from_xml(xml_path, capture_path)


def self_test():
    """The formatters on a hand-made frame (no RenderDoc needed)"""
    root = new_group("Frame", 0)
    scene = new_group("Scene", 1)
    scene["draws"] = 14
    scene["triangles"] = 13308
    scene["renderTargets"] = ["1920x1080 R8G8B8A8_UNORM", "depth 1920x1080"]
    thumbnail = new_group("Thumbnail: coin.prefab.json", 1)
    thumbnail["draws"] = 2
    thumbnail["triangles"] = 200
    ui = new_group("UI", 1)
    ui["draws"] = 28
    ui["triangles"] = 2688
    root["children"] = [thumbnail, scene, ui]
    root["clears"] = 1
    report = {
        "capture": "selftest.rdc",
        "api": "D3D12",
        "eventCount": 120,
        "apiCalls": {"ID3D12GraphicsCommandList::ResourceBarrier": 4, "ID3D12GraphicsCommandList::DrawIndexedInstanced": 44},
        "pipelines": ["pso1", "pso2"],
        "shaders": ["Vertex: VSMain", "Pixel: PSMain"],
        "root": root,
    }
    report["warnings"] = make_warnings(report)

    total = totals(root)
    assert total["draws"] == 44, total
    assert total["triangles"] == 13308 + 200 + 2688, total
    assert flatten(root)[1]["name"] == "Thumbnail: coin.prefab.json"
    markdown = format_markdown(report)
    assert "| Scene | 14 |" in markdown.replace("&nbsp;", ""), markdown
    assert "## Warnings" in markdown and "- none" in markdown
    data = json.loads(to_json(report))
    assert data["root"]["totals"]["draws"] == 44
    assert data["root"]["children"][1]["totals"]["triangles"] == 13308

    empty = new_group("Frame", 0)
    warnings = make_warnings({"root": empty, "apiCalls": {}, "pipelines": []})
    assert any("no marker groups" in warning for warning in warnings), warnings
    assert any("no draw calls" in warning for warning in warnings), warnings
    print("frame_report self test: ok")
    return 0


def main(argv):
    if "--self-test" in argv:
        return self_test()

    capture_path = None
    json_path = None
    markdown_path = None
    index = 0
    while index < len(argv):
        argument = argv[index]
        if argument == "--json" and index + 1 < len(argv):
            json_path = argv[index + 1]
            index += 2
        elif argument == "--markdown" and index + 1 < len(argv):
            markdown_path = argv[index + 1]
            index += 2
        elif argument.startswith("--"):
            sys.stderr.write("unknown option %s\n" % argument)
            return 2
        else:
            capture_path = argument
            index += 1

    if not capture_path:
        sys.stderr.write(__doc__)
        return 2
    if not os.path.isfile(capture_path):
        sys.stderr.write("capture not found: %s\n" % capture_path)
        return 2

    renderdoc_dir = find_renderdoc_dir()
    if renderdoc_dir and renderdoc_dir not in sys.path:
        sys.path.append(renderdoc_dir)
        if hasattr(os, "add_dll_directory"):
            try:
                os.add_dll_directory(renderdoc_dir)
            except OSError:
                pass

    renderdoccmd = find_renderdoccmd(renderdoc_dir)
    try:
        import renderdoc as rd
    except ImportError:
        rd = None

    try:
        if rd is not None:
            report = report_from_capture(rd, capture_path)
        elif renderdoccmd:
            report = report_from_renderdoccmd(capture_path, renderdoccmd)
        else:
            raise RuntimeError("RenderDoc was not found: install it (winget install BaldurKarlsson.RenderDoc) "
                               "or set RENDERDOC_DIR to its folder")
        markdown = write_outputs(report, json_path, markdown_path)
        if not markdown_path:
            sys.stdout.write(markdown + "\n")
        return 0
    except Exception as error:
        sys.stderr.write("frame_report: %s\n" % error)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
