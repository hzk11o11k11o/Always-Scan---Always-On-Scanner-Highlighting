unit TR_Export;

// ===========================================================================
//  Translation helper (export stage) for the Dark Universe Creations.
//
//  Dumps EVERY string-typed element (dtString / dtLString / dtLenString) of
//  every record of every plugin whose name starts with "du_" into one TSV:
//
//     file  fid  ord  recsig  dt  path  text
//
//  "ord" is the ordinal of the string inside that record, in the very same
//  order Walk() visits them.  The import script walks the same way and uses
//  (fid, ord) as the key, so it does not need any knowledge of record layouts.
//
//  Notes:
//   - this file MUST stay pure ASCII and MUST NOT contain brace characters
//     (xEdit reads .pas as ANSI and Pascal brace comments end at the first
//     closing brace -- both traps already cost us a build in this project).
//   - work is done in Initialize so that Process() is a no-op; we therefore
//     never pay for walking the 3.8M records of Starfield.esm.
// ===========================================================================

const
  OutFile = 'D:\workspace\starfield mod\always scan\tr\out\strings.tsv';

var
  sl: TStringList;
  curFile: string;
  curFid: Cardinal;
  curSig: string;
  curIdx: Integer;

function Esc(s: string): string;
var
  i: Integer;
  c: Char;
  r: string;
begin
  r := '';
  for i := 1 to Length(s) do begin
    c := s[i];
    if c = '\' then r := r + '\\'
    else if c = #9 then r := r + '\t'
    else if c = #13 then r := r + '\r'
    else if c = #10 then r := r + '\n'
    else if (Ord(c) >= 32) and (Ord(c) < 127) then r := r + c
    else r := r + '\x' + IntToHex(Ord(c), 4);
  end;
  Result := r;
end;

function IsStrType(e: IInterface): Boolean;
begin
  Result := (DefType(e) = dtString) or (DefType(e) = dtLString) or (DefType(e) = dtLenString);
end;

function DtName(e: IInterface): string;
begin
  if DefType(e) = dtString then Result := 's'
  else if DefType(e) = dtLString then Result := 'l'
  else if DefType(e) = dtLenString then Result := 'n'
  else Result := '?';
end;

procedure Walk(e: IInterface);
var
  i: Integer;
begin
  if not Assigned(e) then Exit;
  if IsStrType(e) then begin
    sl.Add(curFile + #9 + Format('%08X', [curFid]) + #9 + IntToStr(curIdx) + #9 +
           curSig + #9 + DtName(e) + #9 + PathName(e) + #9 + Esc(GetEditValue(e)));
    Inc(curIdx);
    Exit;
  end;
  for i := 0 to Pred(ElementCount(e)) do
    Walk(ElementByIndex(e, i));
end;

function Initialize: Integer;
var
  i, j: Integer;
  f: IwbFile;
  r: IInterface;
  fn: string;
begin
  sl := TStringList.Create;
  sl.Add('#file'#9'fid'#9'ord'#9'recsig'#9'dt'#9'path'#9'text');
  for i := 0 to Pred(FileCount) do begin
    f := FileByIndex(i);
    fn := GetFileName(f);
    if (Length(fn) < 3) or (Copy(fn, 1, 3) <> 'du_') then Continue;
    sl.Add('# file ' + fn + ' records=' + IntToStr(RecordCount(f)));
    for j := 0 to Pred(RecordCount(f)) do begin
      r := RecordByIndex(f, j);
      curFile := fn;
      curFid := GetLoadOrderFormID(r);
      curSig := Signature(r);
      curIdx := 0;
      Walk(r);
    end;
  end;
  sl.SaveToFile(OutFile);
  sl.Free;
  Result := 1;
end;

function Process(e: IInterface): Integer;
begin
  Result := 1;
end;

function Finalize: Integer;
begin
  Result := 0;
end;

end.
