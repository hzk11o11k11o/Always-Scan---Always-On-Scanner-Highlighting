unit UserScript;

const
  OutDir = 'C:\Users\huangzhe\AppData\Local\Temp\kilo\sf-highlight\';

var
  sl: TStringList;

function SigName(e: IInterface): string;
begin
  Result := Signature(e);
end;

procedure DumpRecord(tag: string; r: IInterface);
var
  name: string;
begin
  name := GetElementEditValues(r, 'FULL');
  if name = '' then
    name := GetElementEditValues(r, 'RNAM');
  sl.Add(Format('%s|%s|%s|%s', [tag, IntToHex(GetLoadOrderFormID(r), 8), GetElementEditValues(r, 'EDID'), name]));
end;

procedure DumpAll(f: IwbFile; sig: string; maxCount: Integer);
var
  g: IInterface;
  r: IInterface;
  i, n: Integer;
begin
  g := GroupBySignature(f, sig);
  if not Assigned(g) then begin
    sl.Add(sig + '|absent|-|-');
    Exit;
  end;
  n := ElementCount(g);
  sl.Add(Format('%s|count|%d|-', [sig, n]));
  for i := 0 to Pred(n) do begin
    if maxCount >= 0 then
      if i >= maxCount then begin
        sl.Add(Format('%s|truncated|%d|-', [sig, n - maxCount]));
        Break;
      end;
    r := ElementByIndex(g, i);
    DumpRecord(sig, r);
  end;
end;

function Initialize: Integer;
var
  i: Integer;
  f: IwbFile;
begin
  sl := TStringList.Create;
  sl.Add('=== dump begin ===');

  for i := 0 to Pred(FileCount) do begin
    f := FileByIndex(i);
    sl.Add(Format('file|%d|%s', [i, GetFileName(f)]));
  end;

  f := FileByIndex(0);  // Starfield.esm

  DumpAll(f, 'EFSH', -1);   // 全量：特效着色器
  DumpAll(f, 'KYWD', -1);   // 全量：关键词（后面 grep ObjectType）
  DumpAll(f, 'FLST', -1);   // 全量：FormList

  // 各物品类型：全量倾泻编辑ID，便于后续建表
  DumpAll(f, 'MISC', -1);
  DumpAll(f, 'WEAP', -1);
  DumpAll(f, 'ARMO', -1);
  DumpAll(f, 'ALCH', -1);
  DumpAll(f, 'BOOK', -1);
  DumpAll(f, 'KEYM', -1);
  DumpAll(f, 'AMMO', -1);
  DumpAll(f, 'CONT', -1);
  DumpAll(f, 'DOOR', -1);
  DumpAll(f, 'ACTI', -1);
  DumpAll(f, 'FLOR', -1);
  DumpAll(f, 'FURN', -1);
  DumpAll(f, 'INGR', -1);
  DumpAll(f, 'NPC_', 200);

  sl.Add('=== dump end ===');
  sl.SaveToFile(OutDir + 'dump_records.txt');
  sl.Free;
  Result := 0;
end;

end.
