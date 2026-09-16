unit UserScript;

// Headless xEdit search for "guide path" related records in Starfield.esm.
// Prints every record whose EDID contains "guide" (MGEF/SPEL/QUST/ACTI/FLST/KYWD/MISC)
// plus, for each SPEL, its effect list so we can see who uses ScannerGuideEffect.
//
// Output: <TempOut>\guide_dump.txt   (ASCII, pipe separated)
//
// NOTE: keep this file pure ASCII and never write a brace character (see build_sas.pas).

const
  TempOut = 'C:\Users\huangzhe\AppData\Local\Temp\kilo\sf-alwaysscan\out\';

var
  sl: TStringList;

function Lower(s: string): string;
var
  i: Integer;
begin
  Result := s;
  for i := 1 to Length(Result) do
    if (Result[i] >= 'A') and (Result[i] <= 'Z') then
      Result[i] := Chr(Ord(Result[i]) + 32);
end;

procedure ScanGroup(f: IwbFile; sig: string);
var
  g, r: IInterface;
  i, n, hits: Integer;
  edid: string;
begin
  g := GroupBySignature(f, sig);
  if not Assigned(g) then begin
    sl.Add(sig + '|group absent');
    Exit;
  end;
  n := ElementCount(g);
  hits := 0;
  for i := 0 to Pred(n) do begin
    r := ElementByIndex(g, i);
    edid := GetElementEditValues(r, 'EDID');
    if Pos('guide', Lower(edid)) = 0 then
      Continue;
    Inc(hits);
    sl.Add(sig + '|' + IntToHex(GetLoadOrderFormID(r), 8) + '|' + edid + '|' +
           GetElementEditValues(r, 'FULL'));
  end;
  sl.Add(sig + '|total=' + IntToStr(n) + '|hits=' + IntToStr(hits));
end;

procedure ScanSpellEffects(f: IwbFile);
var
  g, r, effs, eff: IInterface;
  i, n, j, m: Integer;
  line: string;
begin
  g := GroupBySignature(f, 'SPEL');
  if not Assigned(g) then
    Exit;
  n := ElementCount(g);
  for i := 0 to Pred(n) do begin
    r := ElementByIndex(g, i);
    effs := ElementByName(r, 'Effects');
    if not Assigned(effs) then
      Continue;
    m := ElementCount(effs);
    if m = 0 then
      Continue;
    line := '';
    for j := 0 to Pred(m) do begin
      eff := ElementByIndex(effs, j);
      line := line + ' [' + GetElementEditValues(eff, 'EFID') + ']';
    end;
    if Pos('Guide', line) > 0 then
      sl.Add('SPELUSER|' + IntToHex(GetLoadOrderFormID(r), 8) + '|' +
             GetElementEditValues(r, 'EDID') + '|' + line);
  end;
end;

function Initialize: Integer;
var
  i: Integer;
  f: IwbFile;
begin
  sl := TStringList.Create;
  sl.Add('=== guide dump begin ===');
  for i := 0 to Pred(FileCount) do
    sl.Add('file|' + IntToStr(i) + '|' + GetFileName(FileByIndex(i)));

  f := FileByIndex(0);

  ScanGroup(f, 'MGEF');
  ScanGroup(f, 'SPEL');
  ScanGroup(f, 'QUST');
  ScanGroup(f, 'ACTI');
  ScanGroup(f, 'FLST');
  ScanGroup(f, 'KYWD');
  ScanGroup(f, 'MISC');

  sl.Add('=== spell users (Effects list mentions Guide) ===');
  ScanSpellEffects(f);

  sl.Add('=== guide dump end ===');
  sl.SaveToFile(TempOut + 'guide_dump.txt');
  Result := 0;
end;

function Process(e: IInterface): Integer;
begin
  Result := 0;
end;

function Finalize: Integer;
begin
  sl.Free;
  Result := 0;
end;

end.
