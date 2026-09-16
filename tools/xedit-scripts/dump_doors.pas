unit UserScript;

{
  Dump the doors of one interior cell (v2.6 follow-up).

  v2 of the script: the first run found the cell (CityAkilaCityTheRock) but the
  reference walk threw.  This version keeps every step in its own try/except and
  writes a progress marker after each one, so the failing step is identifiable
  from the output file alone.
}

const
  OutDir      = 'd:\workspace\starfield mod\always scan\out\';
  OutFile     = 'doors_dump.txt';
  TargetCell  = $00227BA5;   // CityAkilaCityTheRock (myCell from SAS log)

var
  sl: TStringList;
  cellFound: Boolean;
  refs, doors, xtels: Integer;

procedure Step(s: string);
begin
  sl.Add(s);
  sl.SaveToFile(OutDir + OutFile);
end;

function SafeEdid(e: IInterface): string;
begin
  Result := '';
  try
    Result := GetElementEditValues(e, 'EDID');
  except
    Result := 'ERR';
  end;
end;

function RefInfo(r: IInterface): string;
var
  bsig, beid, pos, tel, cellsig, celleid: string;
  b: IInterface;
begin
  bsig := 'none'; beid := ''; pos := ''; tel := '';
  try
    b := ElementBySignature(r, 'NAME');
    if Assigned(b) then
      if Assigned(LinksTo(b)) then begin
        bsig := Signature(LinksTo(b));
        beid := SafeEdid(LinksTo(b));
      end;
  except
    bsig := 'ERR';
  end;
  try
    if Assigned(ElementByPath(r, 'DATA\Position\X')) then
      pos := Format('%s,%s,%s', [
        FormatFloat('0.0', GetElementNativeValues(r, 'DATA\Position\X')),
        FormatFloat('0.0', GetElementNativeValues(r, 'DATA\Position\Y')),
        FormatFloat('0.0', GetElementNativeValues(r, 'DATA\Position\Z'))]);
  except
    pos := 'ERR';
  end;
  try
    if Assigned(ElementByPath(r, 'XTEL')) then begin
      Inc(xtels);
      cellsig := '?'; celleid := '?';
      if Assigned(ElementByPath(r, 'XTEL\Teleport Link')) then begin
        cellsig := 'path-ok';
        if Assigned(LinksTo(ElementByPath(r, 'XTEL\Teleport Link'))) then
          celleid := SafeEdid(LinksTo(ElementByPath(r, 'XTEL\Teleport Link')));
      end else
        cellsig := 'no-path';
      tel := 'XTEL(' + cellsig + '->' + celleid + ')';
    end;
  except
    tel := 'XTEL(ERR)';
  end;
  Result := Format('%s|base=%s|baseEDID=%s|pos=%s|%s', [
    IntToHex(GetLoadOrderFormID(r), 8), bsig, beid, pos, tel]);
end;

procedure ScanRefs(e: IInterface; depth: Integer);
var
  i, n: Integer;
  c: IInterface;
  sig, entry: string;
begin
  try
    n := ElementCount(e);
  except
    Step('  ERR: ElementCount at depth ' + IntToStr(depth));
    Exit;
  end;
  for i := 0 to Pred(n) do begin
    try
      c := ElementByIndex(e, i);
      sig := Signature(c);
      if sig = 'GRUP' then begin
        if depth < 5 then
          ScanRefs(c, depth + 1);
      end else if (sig = 'REFR') or (sig = 'ACHR') or (sig = 'PGRE') or
                  (sig = 'PMIS') or (sig = 'PHZD') or (sig = 'PARW') or
                  (sig = 'PBAR') or (sig = 'PBEA') or (sig = 'PCON') or
                  (sig = 'PFLA') or (sig = 'APPA') then begin
        Inc(refs);
        entry := sig + '|' + RefInfo(c);
        if (Pos('base=DOOR', entry) > 0) or (Pos('XTEL(', entry) > 0) then begin
          if Pos('base=DOOR', entry) > 0 then Inc(doors);
          sl.Add('  ' + entry);
        end;
        if (refs mod 200) = 0 then
          sl.SaveToFile(OutDir + OutFile);
      end;
    except
      sl.Add('  EXC at ref index ' + IntToStr(i) + ' depth ' + IntToStr(depth));
    end;
  end;
end;

procedure DumpCell(cell: IInterface);
var
  cg: IInterface;
begin
  cellFound := True;
  Step('CELL|' + IntToHex(GetLoadOrderFormID(cell), 8) + '|EDID=' + SafeEdid(cell) +
       '|FULL=' + GetElementEditValues(cell, 'FULL'));
  refs := 0; doors := 0; xtels := 0;
  try
    cg := ChildGroup(cell);
  except
    cg := nil;
  end;
  if Assigned(cg) then begin
    try
      Step('childgroup|count=' + IntToStr(ElementCount(cg)) + '|walking...');
    except
      Step('childgroup|count=ERR');
    end;
    ScanRefs(cg, 0);
  end else
    Step('childgroup|absent');
  Step('cell-record walk...');
  ScanRefs(cell, 0);
  Step(Format('SUMMARY|refs=%d|doorrefs=%d|xtelrefs=%d', [refs, doors, xtels]));
end;

procedure WalkGroup(g: IInterface);
var
  i, n: Integer;
  c: IInterface;
begin
  if not Assigned(g) then
    Exit;
  n := ElementCount(g);
  for i := 0 to Pred(n) do begin
    if cellFound then
      Exit;
    c := ElementByIndex(g, i);
    if Signature(c) = 'GRUP' then
      WalkGroup(c)
    else if GetLoadOrderFormID(c) = TargetCell then
      DumpCell(c);
  end;
end;

function Initialize: Integer;
var
  f, g: IInterface;
  i: Integer;
begin
  sl := TStringList.Create;
  refs := 0; doors := 0; xtels := 0;
  sl.Add('=== doors dump begin ===');
  for i := 0 to Pred(FileCount) do
    sl.Add('file|' + IntToStr(i) + '|' + GetFileName(FileByIndex(i)));

  cellFound := False;
  f := FileByIndex(0);
  try
    g := GroupBySignature(f, 'CELL');
    if not Assigned(g) then
      Step('CELL group absent')
    else
      WalkGroup(g);
    if not cellFound then
      Step('cell ' + IntToHex(TargetCell, 8) + ' NOT FOUND');
  except
    Step('EXCEPTION during walk');
  end;

  Step('=== end ===');
  sl.Free;
  Result := 0;
end;

end.
