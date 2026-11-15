import re
from pathlib import Path
text=Path('.tmp/coverage_final3.txt').read_text()
pairs=re.findall(r'(?m)^(/home/.*\.(?:cpp|hpp))\n\s+(\d+)\s+(\d+)\s+(\d+)%',text)
rows=[(100-int(p),int(l)-int(c),int(l),int(c),int(p),p) for _,l,c,p in pairs]
for row in sorted(rows, reverse=True)[:80]: print(f'{row[5]}: {row[3]}/{row[2]} ({100-row[0]}%), missing={row[1]}')
