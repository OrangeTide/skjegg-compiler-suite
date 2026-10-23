{ a `label` value cannot be a record field }
program err_label_field;
type
  R = record
    where: label;
  end;
var
  r: R;
begin
end.
