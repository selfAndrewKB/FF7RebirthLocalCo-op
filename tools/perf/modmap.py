"""Map captured main.dll (v0.62.0 'third', 7EFE6889..., no matching PDB) RVAs to the current build DLL (age-56 PDB)
with tools/version_match's masked-pattern Matcher; writes modmap.json {rva_captured: [rva_current, how]}.
usage: python modmap.py <dumpdir> <captured main.dll copy> <current LocalSplitScreen.dll>; then LSS_MODDLL=<current DLL>."""
import sys, os, json, collections
sys.path.insert(0, r'C:\_Projects\FinalFantasy7Modding\Rebirth\LocalSplitScreen\tools\version_match')
from pe_image import Image, IDA_BASE
from match import Matcher
os.environ['LSS_DUMP'] = sys.argv[1]
from etwlib import modules, stacks
mods = modules(); ST = stacks(mods)
rvas = sorted({f[1] for fr in ST.values() for f in fr if f[0] == 'mod:LocalSplitScreen'})
old, new = Image(sys.argv[2]), Image(sys.argv[3])  # captured main.dll copy, current build DLL (PDB beside it)
m = Matcher(old, new); out = {}; hows = collections.Counter()
for r in rvas:
    try:
        rg = m.match_code(IDA_BASE + r)
    except Exception as e:
        rg = None
    if rg:
        out[r] = [IDA_BASE + r - rg.old + rg.new - IDA_BASE, rg.how]; hows[rg.how.split(',')[0]] += 1
    else:
        out[r] = [None, 'unmatched']; hows['unmatched'] += 1
json.dump(out, open('modmap.json', 'w'))
print(len(rvas), 'mod RVAs;', dict(hows))
