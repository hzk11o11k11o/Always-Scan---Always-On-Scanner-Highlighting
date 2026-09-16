unit UserScript;

{
  Dump one cell's references (for the v35 corpse investigation).

  Why: the in-game DLL log says "corpse=21/0" -- 21 ACHR refs were seen near the
  player but NONE of them had Actor::boolBits.kDead (+0x208 / bit 0x800) set,
  while the player was standing right next to a lootable body named
  "support personnel".  Two possibilities:
    (a) the visible body is an ACHR that the engine does NOT mark as dead, or
    (b) the visible body is a plain REFR whose base record is an NPC_ --
        Starfield uses those as "posed corpse props".
  This script answers it from static data: for the target cell it prints
    * total reference count,
    * how many ACHR refs the cell has,
    * how many REFR refs have an NPC_ / LVLN / LVLI base,
    * every reference within Radius units of the given point (the player
      position read from SHF_Highlight.log).

  Output is plain ASCII, '|' separated, written to OutDir + OutFile.
}

const
  OutDir      = 'C:\Users\huangzhe\AppData\Local\Temp\kilo\sf-highlight\';
  OutFile     = 'cell_refs.txt';
  TargetID    = $00227BA5;   // cell formID (load order) from SHF_Highlight.log
  Px          = -67.4;       // player position from the log (world units)
  Py          =  49.2;
  Pz          = -140.4;
  Radius      = 80.0;        // units (~23 m); keeps the dump small

var
  sl: TStringList;
  totalRefs : Integer;
  achrCount : Integer;
  npcProp   : Integer;
  refrCount : Integer;
  cellFound : Boolean;

function RefDist(r: IInterface): Double;
var
  x, y, z: Double;
begin
  Result := -1.0;
  if not Assigned(ElementByPath(r, 'DATA\Position\X')) then
    Exit;
  x := GetElementNativeValues(r, 'DATA\Position\X');
  y := GetElementNativeValues(r, 'DATA\Position\Y');
  z := GetElementNativeValues(r, 'DATA\Position\Z');
  Result := Sqrt(Sqr(x - Px) + Sqr(y - Py) + Sqr(z - Pz));
end;

procedure DumpRef(tag: string; r: IInterface);
var
  d: Double;
  base: IInterface;
  bsig: string;
begin
  d := RefDist(r);
  base := ElementBySignature(r, 'NAME');
  bsig := 'none';
  if Assigned(base) then
    if Assigned(LinksTo(base)) then
      bsig := Signature(LinksTo(base));
  sl.Add(Format('%s|%s|%s|base=%s|baseid=%s|dist=%s', [
    tag,
    Signature(r),
    IntToHex(GetLoadOrderFormID(r), 8),
    bsig,
    GetElementEditValues(r, 'NAME'),
    FormatFloat('0.0', d)]));
end;

procedure ScanRefs(e: IInterface; depth: Integer);
var
  i, n: Integer;
  c, base: IInterface;
  sig, bsig: string;
  d: Double;
begin
  n := ElementCount(e);
  for i := 0 to Pred(n) do begin
    c := ElementByIndex(e, i);
    if Signature(c) = 'GRUP' then begin
      if depth < 4 then
        ScanRefs(c, depth + 1);
    end else begin
      sig := Signature(c);
      if (sig = 'REFR') or (sig = 'ACHR') or (sig = 'PGRE') or (sig = 'PMIS') or
         (sig = 'PHZD') or (sig = 'PARW') or (sig = 'PBAR') or (sig = 'PBEA') or
         (sig = 'PCON') or (sig = 'PFLA') or (sig = 'APPA') then begin
        Inc(totalRefs);
        base := ElementBySignature(c, 'NAME');
        bsig := '';
        if Assigned(base) then
          if Assigned(LinksTo(base)) then
            bsig := Signature(LinksTo(base));
        if sig = 'ACHR' then
          Inc(achrCount);
        if sig = 'REFR' then begin
          Inc(refrCount);
          if (bsig = 'NPC_') or (bsig = 'LVLN') or (bsig = 'LVLI') then
            Inc(npcProp);
        end;
        d := RefDist(c);
        if (d >= 0.0) and (d <= Radius) then
          DumpRef('near', c);
      end;
    end;
  end;
end;

procedure DumpCell(cell: IInterface);
var
  cg: IInterface;
begin
  totalRefs := 0; achrCount := 0; npcProp := 0; refrCount := 0;
  cellFound := True;
  sl.Add(Format('CELL|%s|EDID=%s|FULL=%s', [
    IntToHex(GetLoadOrderFormID(cell), 8),
    GetElementEditValues(cell, 'EDID'),
    GetElementEditValues(cell, 'FULL')]));
  // 引用记录不在 CELL 记录内部，而在它的「子组」里（Cell Children / Persistent /
  // Temporary）：xEdit 用 ChildGroup() 取。第一版脚本没取，才会 refs=0。
  cg := ChildGroup(cell);
  if Assigned(cg) then begin
    sl.Add(Format('childgroup|count=%d', [ElementCount(cg)]));
    ScanRefs(cg, 0);
  end else
    sl.Add('childgroup|absent');
  ScanRefs(cell, 0);
  sl.Add(Format('SUMMARY|refs=%d|achr=%d|refr=%d|refr_npcbase=%d', [
    totalRefs, achrCount, refrCount, npcProp]));
  sl.SaveToFile(OutDir + OutFile);
end;

procedure WalkGroup(g: IInterface; depth: Integer);
var
  i, n: Integer;
  c: IInterface;
begin
  if not Assigned(g) then
    Exit;
  if cellFound then
    Exit;
  n := ElementCount(g);
  for i := 0 to Pred(n) do begin
    if cellFound then
      Exit;
    c := ElementByIndex(g, i);
    if Signature(c) = 'GRUP' then
      WalkGroup(c, depth + 1)
    else if GetLoadOrderFormID(c) = TargetID then
      DumpCell(c);
  end;
end;

function Initialize: Integer;
var
  f, g: IInterface;
  i: Integer;
begin
  sl := TStringList.Create;
  sl.Add('=== cell refs dump begin ===');
  for i := 0 to Pred(FileCount) do
    sl.Add('file|' + IntToStr(i) + '|' + GetFileName(FileByIndex(i)));

  cellFound := False;
  f := FileByIndex(0);
  sl.Add('stage|walk-start');
  sl.SaveToFile(OutDir + OutFile);   // stage write: proves we got here
  try
    g := GroupBySignature(f, 'CELL');
    if not Assigned(g) then
      sl.Add('CELL group absent')
    else
      WalkGroup(g, 0);
    if not cellFound then
      sl.Add('cell ' + IntToHex(TargetID, 8) + ' NOT FOUND');
  except
    sl.Add('EXCEPTION during walk');
  end;

  sl.Add('=== end ===');
  sl.SaveToFile(OutDir + OutFile);
  sl.Free;
  Result := 0;
end;

end.
