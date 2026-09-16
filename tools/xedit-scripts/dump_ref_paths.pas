unit UserScript;

{
  Dump every subrecord path of specific references (v2.6 follow-up).

  Why: the three load doors in CityAkilaCityTheRock (00220686/00220687/00220688)
  carry an XTEL, but ElementByPath(r, 'XTEL\Teleport Link') found nothing, so the
  path name used by the previous dump script was wrong.  Papyrus GetTeleportCell()
  reads the same data, so its exact layout matters here.

  v2: only walk the target cell (the previous version walked every CELL in
  Starfield.esm and was killed before finishing) + write a separate .done marker
  so run-xedit.ps1 does not close xEdit the moment the output file appears.
}

const
  OutDir  = 'd:\workspace\starfield mod\always scan\out\';
  OutFile = 'ref_paths.txt';
  DoneFile = 'ref_paths.done';
  TargetCell = $00227BA5;   // CityAkilaCityTheRock

var
  sl: TStringList;
  targets: string;
  found: Boolean;

procedure Walk(e: IInterface; prefix: string; depth: Integer);
var
  i, n: Integer;
  c: IInterface;
  nm, val: string;
begin
  if depth > 6 then
    Exit;
  n := ElementCount(e);
  if n = 0 then begin
    sl.Add(prefix + ' = ' + GetEditValue(e));
    Exit;
  end;
  for i := 0 to Pred(n) do begin
    c := ElementByIndex(e, i);
    nm := Name(c);
    try
      val := GetEditValue(c);
    except
      val := 'ERR';
    end;
    if ElementCount(c) = 0 then
      sl.Add(prefix + '\' + nm + ' = ' + val)
    else begin
      sl.Add(prefix + '\' + nm + '  (children)  = ' + val);
      Walk(c, prefix + '\' + nm, depth + 1);
    end;
  end;
end;

procedure DumpTarget(prefix: string; c: IInterface);
var
  fid: Cardinal;
  hex: string;
begin
  fid := GetLoadOrderFormID(c);
  hex := '|' + IntToHex(fid and $FFFFFF, 8) + '|';
  if Pos(hex, targets) > 0 then begin
    sl.Add('== ' + prefix + ' ' + Signature(c) + ' ' + IntToHex(fid, 8) + ' ==');
    Walk(c, '', 0);
    sl.Add('== end ==');
    sl.SaveToFile(OutDir + OutFile);
  end;
end;

procedure ScanFlat(e: IInterface; prefix: string; depth: Integer);
var
  i, n: Integer;
  c: IInterface;
begin
  if depth > 6 then Exit;
  n := ElementCount(e);
  for i := 0 to Pred(n) do begin
    c := ElementByIndex(e, i);
    if Signature(c) = 'GRUP' then
      ScanFlat(c, prefix, depth + 1)
    else
      DumpTarget(prefix, c);
  end;
end;

procedure WalkCells(g: IInterface; depth: Integer);
var
  i, n: Integer;
  c: IInterface;
begin
  if found or (depth > 6) then Exit;
  n := ElementCount(g);
  for i := 0 to Pred(n) do begin
    if found then Exit;
    c := ElementByIndex(g, i);
    if Signature(c) = 'GRUP' then
      WalkCells(c, depth + 1)
    else if GetLoadOrderFormID(c) = TargetCell then begin
      found := True;
      sl.Add('cell found: ' + IntToHex(TargetCell, 8));
      sl.SaveToFile(OutDir + OutFile);
      if Assigned(ChildGroup(c)) then
        ScanFlat(ChildGroup(c), 'cgref', 0);
      ScanFlat(c, 'cellref', 0);
    end;
  end;
end;

function Initialize: Integer;
var
  f, g: IInterface;
  i: Integer;
begin
  sl := TStringList.Create;
  targets := '|00220688|00220686|00220687|0013E58C|001D5AB7|';
  for i := 0 to Pred(FileCount) do
    sl.Add('file|' + IntToStr(i) + '|' + GetFileName(FileByIndex(i)));
  sl.SaveToFile(OutDir + OutFile);
  f := FileByIndex(0);
  try
    sl.Add('--- walking CELL group for the target cell ---');
    sl.SaveToFile(OutDir + OutFile);
    WalkCells(GroupBySignature(f, 'CELL'), 0);
    sl.Add('--- walking DOOR group for base records ---');
    sl.SaveToFile(OutDir + OutFile);
    ScanFlat(GroupBySignature(f, 'DOOR'), 'doorbase', 0);
  except
    sl.Add('EXCEPTION during scan');
  end;
  sl.Add('=== end ===');
  sl.SaveToFile(OutDir + OutFile);
  sl.SaveToFile(OutDir + DoneFile);
  sl.Free;
  Result := 0;
end;

end.
