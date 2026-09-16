unit UserScript;

const
  OutDir = 'C:\Users\huangzhe\AppData\Local\Temp\kilo\sf-highlight\';

var
  slOut: TStringList;

function Initialize: Integer;
var
  i: Integer;
begin
  slOut := TStringList.Create;
  slOut.Add('started');
  for i := 0 to Pred(FileCount) do
    slOut.Add('Loaded: ' + GetFileName(FileByIndex(i)));
  slOut.SaveToFile(OutDir + 'smoke_started.txt');
  Result := 0;
end;

function Process(e: IInterface): Integer;
begin
  Result := 0;
end;

function Finalize: Integer;
begin
  slOut.Add('finished');
  slOut.SaveToFile(OutDir + 'smoke_finished.txt');
  slOut.Free;
end;

end.
