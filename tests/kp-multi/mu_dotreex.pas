program mu_dotreex;
{ one `uses dx` fans out the dotted leaf dx.base through the umbrella's
  re-export; this program never names dx.base directly. }
uses dx as _;
begin
  WriteLn('scale=', Scale(5));
  WriteLn('half=', Half);
end.
