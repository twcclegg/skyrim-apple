import sys, re
path, size, msaa = sys.argv[1], sys.argv[2], sys.argv[3]
want = {
 "Display": {"iMultiSample": msaa, "iMaxAnisotropy": "16", "iTexMipMapSkip": "0", "iShadowMapResolution": "4096",
   "iBlurDeferredShadowMask": "3", "bFXAAEnabled": "0", "fMeshLODLevel2FadeDist": "10000000.0000",
   "fMeshLODLevel1FadeDist": "10000000.0000", "fTreesMidLODSwitchDist": "10000000.0000", "fShadowDistance": "8000.0000",
   "bTreesReceiveShadows": "1", "bDrawLandShadows": "1", "bShadowsOnGrass": "1", "bDrawShadows": "1", "iShadowFilter": "4",
   "iShadowMode": "3", "fSpecularLODStartFade": "5000.0000", "fLightLODStartFade": "5000.0000", "fShadowLODStartFade": "5000.0000",
   "bTransparencyMultisampling": "1", "iWaterMultiSamples": msaa, "iSize W": size, "iSize H": size, "iPresentInterval": "0",
   "bFull Screen": "0"},
 "LOD": {"fLODFadeOutMultActors": "20.0000", "fLODFadeOutMultObjects": "20.0000", "fLODFadeOutMultItems": "20.0000",
   "fLODFadeOutMultSkyCell": "1.0000"},
 "TerrainManager": {"fBlockLevel0Distance": "60000.0000", "fBlockLevel1Distance": "90000.0000",
   "fBlockMaximumDistance": "250000.0000", "fSplitDistanceMult": "1.5000", "fTreeLoadDistance": "75000.0000"},
 "Grass": {"fGrassStartFadeDistance": "7000.0000", "fGrassMaxStartFadeDistance": "7000.0000", "b30GrassVS": "1"},
 "Water": {"iWaterReflectHeight": "2048", "iWaterReflectWidth": "2048", "bUseWaterReflectionBlur": "1",
   "bReflectLODObjects": "1", "bReflectLODLand": "1", "bReflectSky": "1", "bReflectLODTrees": "1", "iWaterBlurAmount": "4",
   "bUseWaterDisplacements": "1", "bUseWaterRefractions": "1", "bUseWaterReflections": "1", "bUseWaterDepth": "1"},
 "Decals": {"uMaxDecals": "1000", "bDecals": "1", "bSkinnedDecals": "1", "uMaxSkinDecals": "100", "uMaxSkinDecalsPerActor": "60"},
 "MAIN": {"fSkyCellRefFadeDistance": "600000.0000"},
 "Imagespace": {"bDoDepthOfField": "1"},
}
lines = open(path, encoding="latin-1").read().splitlines()
out, sec, seen = [], None, set()
def flush(sec):
    for k, v in want.get(sec, {}).items():
        if (sec, k) not in seen: out.append(f"{k}={v}"); seen.add((sec, k))
for ln in lines:
    m = re.match(r"\[(.+)\]", ln.strip())
    if m:
        if sec: flush(sec)
        sec = m.group(1); out.append(ln); continue
    if "=" in ln and sec:
        k = ln.split("=", 1)[0]
        if k in want.get(sec, {}):
            out.append(f"{k}={want[sec][k]}"); seen.add((sec, k)); continue
    out.append(ln)
if sec: flush(sec)
for s, kv in want.items():
    if not any(x[0] == s for x in seen):
        out.append(f"[{s}]"); out += [f"{k}={v}" for k, v in kv.items()]
open(path, "w", encoding="latin-1").write("\r\n".join(out) + "\r\n")
