unit subworddep;

interface

{ a record with sub-word integer fields exported across the unit interface, so
  the consumer resolves byte/word/smallint through the module (TypeRef on the
  producer, RefToType on the importer). }
type Pixel = record
  r: byte;
  g: byte;
  b: byte;
  a: byte;
  depth: word;
  bias: smallint;
end;

function MakePixel(r, g, b, a, depth, bias: integer): Pixel;

implementation

function MakePixel(r, g, b, a, depth, bias: integer): Pixel;
begin
  MakePixel.r := r;
  MakePixel.g := g;
  MakePixel.b := b;
  MakePixel.a := a;
  MakePixel.depth := depth;
  MakePixel.bias := bias;
end;

end.
