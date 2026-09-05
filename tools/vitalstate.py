"""Read/write Vital's VST3 plugin state chunk as plain preset JSON."""
import json, re, struct, juceb64

XML = ('<?xml version="1.0" encoding="UTF-8"?> <VST3PluginState><IComponent>%s'
       '</IComponent><IEditController>%s</IEditController></VST3PluginState>')

def _split(raw):
    """raw_state bytes -> (fxb_header, json_dict, trailing_private_data, ictrl_b64)"""
    xml = raw[8:].decode("utf-8", "replace")
    comp = juceb64.decode(re.search(r"<IComponent>(.*?)</IComponent>", xml, re.S).group(1).strip())
    mc = re.search(r"<IEditController>(.*?)</IEditController>", xml, re.S)
    start = comp.find(b'{"')
    obj, end = json.JSONDecoder().raw_decode(comp[start:].decode("utf-8"))
    return comp[:start], obj, comp[start + end:], (mc.group(1).strip() if mc else "")

def read(raw):
    return _split(raw)[1]

def write(raw, preset):
    """Return a new raw_state with `preset` (a .vital dict) swapped in."""
    header, _, trailing, ictrl = _split(raw)
    body = json.dumps(preset, separators=(",", ":")).encode("utf-8")
    chunk_payload = body + trailing
    header = bytearray(header)
    struct.pack_into(">I", header, 172, len(chunk_payload))          # FXB chunkSize
    comp = bytes(header) + chunk_payload
    struct.pack_into(">I", comp_ba := bytearray(comp), 20, len(comp) - 24)  # FXB byteSize
    comp = bytes(comp_ba)
    xml = XML % (juceb64.encode(comp), ictrl)
    xml_b = xml.encode("utf-8")
    return b"VC2!" + struct.pack("<I", len(xml_b) + 1) + xml_b + b"\x00"
