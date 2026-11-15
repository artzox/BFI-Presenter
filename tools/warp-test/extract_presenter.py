# Writes gen_presenter.h: the presenter's shader source and its constants, taken from dx/bfi_presenter.cpp,
# so the test draws with exactly what the add-on uses.
import re, os
here = os.path.dirname(os.path.abspath(__file__))
s = open(os.path.join(here, '..', '..', 'dx', 'bfi_presenter.cpp'), encoding='utf-8').read()
m = re.search(r'struct Constants\n\{.*?\n\};', s, re.S)
k = re.search(r'static const char kShader\[\] = R"HLSL\(.*?\)HLSL";', s, re.S)
open(os.path.join(here, 'gen_presenter.h'), 'w', encoding='utf-8').write('// extracted from dx/bfi_presenter.cpp\n' + k.group(0) + '\n' + m.group(0) + '\n')
