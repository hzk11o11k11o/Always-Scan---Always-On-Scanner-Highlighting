unit SAS_BuildPlugin;

// ===========================================================================
//  Starfield Always Scan - plugin builder (headless xEdit script)
// ===========================================================================
//
//  !!! THIS FILE MUST STAY PURE ASCII AND MUST NOT CONTAIN ANY BRACE CHARACTER !!!
//
//  Two traps, both hit for real:
//   1. xEdit reads .pas as ANSI (GBK here). A UTF-8 Chinese comment becomes
//      mojibake whose bytes can break the parser ->
//      "Error in unit 'SAS_BuildPlugin' on line N : = expected but '(' found".
//   2. Pascal brace comments end at the FIRST closing brace, so a closing brace
//      written inside a comment silently truncates it ->
//      "Declaration expected but '...' found".
//  tools\build-sas.ps1 refuses to run this file if it finds any byte > 127 or
//  any brace character, so these two can no longer waste a 5 minute build.
//
// ---------------------------------------------------------------------------
//  Record table. Creation order == FormID order (low 24 bits).
//
//    0x800  EFSH SAS_HighlightFXS      <- PLACEHOLDER (old EFSH visual layer)
//    0x801  FLST SAS_OpList            <- PLACEHOLDER (old play mailbox)
//    0x802  GLOB SAS_OpCursor          <- PLACEHOLDER
//    0x803  FLST SAS_StopList          <- PLACEHOLDER (old stop mailbox)
//    0x804  GLOB SAS_StopCursor        <- PLACEHOLDER
//    0x805  GLOB SAS_Epoch             <- PLACEHOLDER (old load-game counter)
//    0x806  QUST SAS_AlwaysScanQuest   (Start Game Enabled + Starts Enabled)
//    0x807  GLOB SAS_Notify            <- v4.0: DLL writes 1/2, script pops HUD
//
//  v4.0 note: every legacy bridge record is now UNUSED -- the DLL drives the
//  engine's native outline directly and no longer needs any of them, and the
//  Papyrus script only (a) cleans up old breadcrumb beads and (b) polls
//  SAS_Notify. They are deliberately KEPT, in this exact order, as FormID
//  placeholders: the quest's FormID must stay 0x806, because a saved game
//  archives the quest's script instance BY FORMID. If it shifted, the restored
//  instance would be dropped and `GuideArrayReady` would read back False ->
//  the one-time bead cleanup would never run again for old saves.
//  New records MUST be appended at the END (0x807+).
//
//  VMAD (bound on the quest):
//    NotifyFlag   (-> 0x807 SAS_Notify)
//
// ---------------------------------------------------------------------------
//  Known traps:
//  1. In Starfield mode xEdit refuses to create .esp -> extension MUST be .esm.
//  2. AddNewFileName creates an empty file in the GAME DATA directory; the real
//     content is written with FileWriteToStream into OutDir.
//  3. FLST entries only resolve when written with SetEditValue(entry, Name(form)).
//     Here we only need to EMPTY the list, so RemoveByIndex is enough.
//  4. The only writable colour on an EFSH is the named struct member
//     DNAM \ Color. Do NOT touch its 4th byte (xEdit calls it 'Unused').
//  5. Quest Flags: 0x01 Start Game Enabled + 0x10 Starts Enabled = 17.
//  6. The .pas must stay PURE ASCII with NO brace characters.
// ===========================================================================

const
  OutDir       = 'C:\Users\huangzhe\AppData\Local\Temp\kilo\sf-alwaysscan\out\';
  PluginName   = 'StarfieldAlwaysScan.esm';
  ScriptBridge = 'SAS_Bridge';

  // Highlight colour (RGB). Only DNAM's named colour members are written.
  ShaderR = 150;
  ShaderG = 230;
  ShaderB = 255;

var
  sl: TStringList;
  srcFile: IwbFile;
  questRec: IInterface;
  esmShader, flstOp, flstStop: IInterface;
  globOpCur, globStopCur, globEpoch, globNotify: IInterface;

procedure Log(s: string);
begin
  sl.Add(s);
end;

procedure Flush(tag: string);
begin
  sl.SaveToFile(OutDir + 'h_' + tag + '.txt');
end;

function TopGroup(f: IwbFile; sig: string): IInterface;
var
  i: Integer;
  g: IInterface;
begin
  Result := nil;
  for i := 0 to Pred(ElementCount(f)) do begin
    g := ElementByIndex(f, i);
    if Name(g) = 'GRUP Top "' + sig + '"' then begin
      Result := g;
      Exit;
    end;
  end;
end;

function FindByEdid(f: IwbFile; sig, edid: string): IInterface;
var
  g, r: IInterface;
  i: Integer;
begin
  Result := nil;
  g := TopGroup(f, sig);
  if not Assigned(g) then Exit;
  for i := 0 to Pred(ElementCount(g)) do begin
    r := ElementByIndex(g, i);
    if GetElementEditValues(r, 'EDID') = edid then begin
      Result := r;
      Exit;
    end;
  end;
end;

function FindFloatGlobalTemplate(): IInterface;
var
  g, r: IInterface;
  i, n, fnam: Integer;
begin
  Result := nil;
  n := 0;
  g := TopGroup(srcFile, 'GLOB');
  if not Assigned(g) then begin
    Log('ERR: no GLOB group');
    Exit;
  end;
  for i := 0 to Pred(ElementCount(g)) do begin
    r := ElementByIndex(g, i);
    if Signature(r) <> 'GLOB' then Continue;
    fnam := GetElementNativeValues(r, 'FNAM');
    if n < 5 then begin
      Log('  glob cand ' + IntToStr(n) + ': ' + GetElementEditValues(r, 'EDID')
        + ' FNAM=' + IntToStr(fnam) + ' FLTV=' + GetElementEditValues(r, 'FLTV'));
      Inc(n);
    end;
    if (fnam = 2) and not Assigned(Result) then
      Result := r;
  end;
  if not Assigned(Result) then begin
    Log('WARN: no float GLOB found, falling back to first record');
    Result := ElementByIndex(g, 0);
  end;
end;

function MakeEFSH(newFile: IwbFile; edid, tplEdid: string; r, g, b: Integer): IInterface;
var
  tpl, rec: IInterface;
begin
  Result := nil;
  Log('  EFSH enter: ' + edid);
  Flush('02b_efsh_enter');
  tpl := FindByEdid(srcFile, 'EFSH', tplEdid);
  if not Assigned(tpl) then begin
    Log('ERR: EFSH template missing: ' + tplEdid);
    Exit;
  end;
  AddRequiredElementMasters(tpl, newFile, False);
  rec := wbCopyElementToFile(tpl, newFile, True, True);
  if not Assigned(rec) then begin
    Log('ERR: EFSH copy failed ' + edid);
    Exit;
  end;
  SetElementEditValues(rec, 'EDID', edid);
  SetElementNativeValues(rec, 'DNAM\Color\Red', r);
  SetElementNativeValues(rec, 'DNAM\Color\Green', g);
  SetElementNativeValues(rec, 'DNAM\Color\Blue', b);
  Log('EFSH ok: ' + edid + ' <- ' + tplEdid + ' ' + IntToHex(GetLoadOrderFormID(rec), 8)
    + ' rgb=' + IntToStr(r) + ',' + IntToStr(g) + ',' + IntToStr(b));
  Result := rec;
end;

function MakeEmptyFLST(newFile: IwbFile; edid: string): IInterface;
var
  tpl, rec, entries: IInterface;
begin
  Result := nil;
  tpl := FindByEdid(srcFile, 'FLST', 'HelpManualPC');
  if not Assigned(tpl) then begin
    Log('ERR: FLST template missing');
    Exit;
  end;
  AddRequiredElementMasters(tpl, newFile, False);
  rec := wbCopyElementToFile(tpl, newFile, True, True);
  if not Assigned(rec) then begin
    Log('ERR: FLST copy failed');
    Exit;
  end;
  SetElementEditValues(rec, 'EDID', edid);

  entries := ElementByName(rec, 'FormIDs');
  if not Assigned(entries) then
    entries := Add(rec, 'FormIDs', True);
  while ElementCount(entries) > 0 do
    RemoveByIndex(entries, 0, True);

  Log('FLST ok: ' + edid + ' entries=' + IntToStr(ElementCount(entries))
    + ' ' + IntToHex(GetLoadOrderFormID(rec), 8));
  Result := rec;
end;

function MakeGlobal(newFile: IwbFile; edid: string; tpl: IInterface; value: Double): IInterface;
var
  rec: IInterface;
begin
  Result := nil;
  if not Assigned(tpl) then begin
    Log('ERR: GLOB template missing');
    Exit;
  end;
  AddRequiredElementMasters(tpl, newFile, False);
  rec := wbCopyElementToFile(tpl, newFile, True, True);
  if not Assigned(rec) then begin
    Log('ERR: GLOB copy failed ' + edid);
    Exit;
  end;
  SetElementEditValues(rec, 'EDID', edid);
  SetElementNativeValues(rec, 'FNAM', 2);
  SetElementNativeValues(rec, 'FLTV', value);
  Log('GLOB ok: ' + edid + ' FLTV=' + GetElementEditValues(rec, 'FLTV')
    + ' ' + IntToHex(GetLoadOrderFormID(rec), 8));
  Result := rec;
end;

function MakeQuestSkeleton(newFile: IwbFile; edid, sname: string): IInterface;
var
  tpl, rec, vmad, scripts, scriptRec: IInterface;
begin
  Result := nil;
  tpl := FindByEdid(srcFile, 'QUST', 'SQ_PlayerHouse');
  if not Assigned(tpl) then begin
    Log('ERR: QUST template missing');
    Exit;
  end;
  AddRequiredElementMasters(tpl, newFile, False);
  rec := wbCopyElementToFile(tpl, newFile, True, True);
  if not Assigned(rec) then begin
    Log('ERR: QUST copy failed');
    Exit;
  end;
  SetElementEditValues(rec, 'EDID', edid);
  SetElementEditValues(rec, 'FULL', 'Always Scan');

  // Start Game Enabled (0x01) + Starts Enabled (0x10)
  SetElementNativeValues(rec, 'DNAM\Flags', 17);

  if ElementExists(rec, 'VMAD') then
    RemoveElement(rec, 'VMAD');
  vmad := Add(rec, 'VMAD', True);
  SetElementNativeValues(vmad, 'Version', 6);
  SetElementNativeValues(vmad, 'Object Format', 2);

  scripts := ElementByPath(vmad, 'Scripts');
  scriptRec := ElementAssign(scripts, HighInteger, nil, False);
  SetElementEditValues(scriptRec, 'ScriptName', sname);
  SetElementNativeValues(scriptRec, 'Flags', 0);

  Log('QUST ok: ' + edid + ' ' + IntToHex(GetLoadOrderFormID(rec), 8));
  Result := rec;
end;

function PropObject(scriptRec: IInterface; pname: string; target: IInterface): Boolean;
var
  props, prop: IInterface;
begin
  Result := False;
  if not Assigned(target) then begin
    Log('  ERR prop target missing: ' + pname);
    Exit;
  end;
  props := ElementByPath(scriptRec, 'Properties');
  prop := ElementAssign(props, HighInteger, nil, False);
  SetElementEditValues(prop, 'propertyName', pname);
  SetElementNativeValues(prop, 'Type', 1);
  SetElementNativeValues(prop, 'Flags', 1);
  SetElementEditValues(prop, 'Value\Object Union\Object v2\FormID', Name(target));
  Log('  prop ' + pname + ' -> ' + Name(target));
  Result := True;
end;

function AddQuestProp(a_quest: IInterface; pname: string; target: IInterface): Boolean;
var
  vmad, scripts, scriptRec: IInterface;
begin
  Result := False;
  if not Assigned(a_quest) then begin
    Log('  ERR AddQuestProp: quest missing (' + pname + ')');
    Exit;
  end;
  vmad := ElementByPath(a_quest, 'VMAD');
  if not Assigned(vmad) then begin
    Log('  ERR AddQuestProp: VMAD missing (' + pname + ')');
    Exit;
  end;
  scripts := ElementByPath(vmad, 'Scripts');
  if not Assigned(scripts) then begin
    Log('  ERR AddQuestProp: Scripts missing (' + pname + ')');
    Exit;
  end;
  scriptRec := ElementByIndex(scripts, 0);
  if not Assigned(scriptRec) then begin
    Log('  ERR AddQuestProp: script entry missing (' + pname + ')');
    Exit;
  end;
  Result := PropObject(scriptRec, pname, target);
end;

// Real body. Wrapped by Initialize() so any exception lands in h_98_exception.txt.
function DoBuild: Integer;
var
  newFile: IwbFile;
  globTpl: IInterface;
  fs: TFileStream;
  i: Integer;
begin
  sl := TStringList.Create;
  Log('=== Always Scan build start ===');
  Flush('00_start');

  srcFile := FileByIndex(0);
  Log('src = ' + GetFileName(srcFile));

  newFile := AddNewFileName(PluginName);
  if not Assigned(newFile) then begin
    Log('FATAL: cannot create ' + PluginName);
    Flush('99_fatal');
    sl.Free;
    Result := 1;
    Exit;
  end;
  SetIsESM(newFile, True);
  Log('plugin created');
  Flush('01_created');

  globTpl := FindFloatGlobalTemplate();
  Flush('02_globtpl');

  // ------- 0x800 -------
  // PLACEHOLDER: keeps the low-24 id 0x800 occupied so the quest stays at 0x806.
  esmShader := MakeEFSH(newFile, 'SAS_HighlightFXS', 'ReconTargetingFXS', ShaderR, ShaderG, ShaderB);
  Flush('03_efsh');

  // ------- 0x801 / 0x802 -------
  // PLACEHOLDER (old play mailbox / cursor).
  flstOp := MakeEmptyFLST(newFile, 'SAS_OpList');
  globOpCur := MakeGlobal(newFile, 'SAS_OpCursor', globTpl, 0.0);
  Flush('04_play');

  // ------- 0x803 / 0x804 -------
  // PLACEHOLDER (old stop mailbox / cursor).
  flstStop := MakeEmptyFLST(newFile, 'SAS_StopList');
  globStopCur := MakeGlobal(newFile, 'SAS_StopCursor', globTpl, 0.0);
  Flush('05_stop');

  // ------- 0x805 -------
  // PLACEHOLDER (old load-game epoch counter; replaced by TESLoadGameEvent).
  globEpoch := MakeGlobal(newFile, 'SAS_Epoch', globTpl, 0.0);
  Flush('06_epoch');

  // ------- 0x806 -------
  questRec := MakeQuestSkeleton(newFile, 'SAS_AlwaysScanQuest', ScriptBridge);
  Flush('07_quest');

  // ------- 0x807 ---- (MUST be after 0x806: new records are appended last) ----
  globNotify := MakeGlobal(newFile, 'SAS_Notify', globTpl, 0.0);
  Flush('08_notify');

  // VMAD properties: added only after all target records exist.
  // v4.0: the DLL no longer touches the mailbox forms, so the only remaining
  // binding is the HUD-notification flag.
  AddQuestProp(questRec, 'NotifyFlag', globNotify);
  Flush('09_vmad');

  try
    SortMasters(newFile);
    Log('masters:');
    for i := 0 to Pred(MasterCount(newFile)) do
      Log('  ' + GetFileName(MasterByIndex(newFile, i)));
  except
    on E: Exception do Log('EXC sort: ' + E.Message);
  end;

  try
    fs := TFileStream.Create(OutDir + PluginName, fmCreate);
    try
      FileWriteToStream(newFile, fs, False);
    finally
      fs.Free;
    end;
    Log('saved: ' + OutDir + PluginName);
  except
    on E: Exception do Log('EXC save: ' + E.Message);
  end;

  // FormID map: must match the kLocal* constants in plugin/src/AlwaysScan.cpp
  Log('--- FormID map (low 24 bits) ---');
  Log('  0x800 SAS_HighlightFXS    ' + IntToHex(GetLoadOrderFormID(esmShader) and $FFFFFF, 6));
  Log('  0x801 SAS_OpList          ' + IntToHex(GetLoadOrderFormID(flstOp) and $FFFFFF, 6));
  Log('  0x802 SAS_OpCursor        ' + IntToHex(GetLoadOrderFormID(globOpCur) and $FFFFFF, 6));
  Log('  0x803 SAS_StopList        ' + IntToHex(GetLoadOrderFormID(flstStop) and $FFFFFF, 6));
  Log('  0x804 SAS_StopCursor      ' + IntToHex(GetLoadOrderFormID(globStopCur) and $FFFFFF, 6));
  Log('  0x805 SAS_Epoch           ' + IntToHex(GetLoadOrderFormID(globEpoch) and $FFFFFF, 6));
  Log('  0x806 SAS_AlwaysScanQuest ' + IntToHex(GetLoadOrderFormID(questRec) and $FFFFFF, 6));
  Log('  0x807 SAS_Notify          ' + IntToHex(GetLoadOrderFormID(globNotify) and $FFFFFF, 6));

  Log('=== Always Scan build done ===');
  Flush('99_done');
  sl.Free;
  Result := 0;
end;

function Initialize: Integer;
begin
  try
    Result := DoBuild();
  except
    on E: Exception do begin
      if not Assigned(sl) then
        sl := TStringList.Create;
      Log('EXCEPTION: ' + E.Message);
      Flush('98_exception');
      Result := 1;
    end;
  end;
end;

end.
