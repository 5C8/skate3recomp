"""Exercise the actual profile callback and migration of the retired movie patch.

The SDK boundary uses simulated config. No game assets or generated game code
are needed: movie fixtures invoke a counting stub for the original call.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, signature):
    masked = re.sub(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/',
                    lambda m: ' ' * len(m.group()), source, flags=re.S)
    start = source.index(signature)
    brace = masked.index('{', start)
    depth = 0
    for i in range(brace, len(source)):
        depth += (masked[i] == '{') - (masked[i] == '}')
        if depth == 0:
            return source[start:i + 1]
    raise ValueError('Unclosed function: ' + signature)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
parser.add_argument('--cxx', default='c++')
parser.add_argument('--build-dir', type=Path)
args = parser.parse_args()
app = (args.root / 'src/skate3_app_common.cpp').read_text()
demo = (args.root / 'src/skate3_demo_path.cpp').read_text()
for retired in ('bool ShouldForceIntroMovieComplete(', 'bool UserRequestedMovieSkip(',
                'void EnableIntroMovieSkipIfNeeded('):
    assert retired not in demo, 'Retired completion override was reintroduced'
callback = function(app, 'void ApplyDemoPathProfileOverride()')
legacy = """void LegacyMovie(Ctx& ctx, void* base) {
\tif (skate3::demo_path::ShouldForceIntroMovieComplete()) {
\t\tctx.r3.u64 = 0;
\t} else {
\t\t// bl 0x825d60c8
\t\tctx.lr = 0x825E05A0;
\t\trex_AptSafeQueueFixed_Enqueue_QAAXPAX_Z_825D60C8(ctx, base);
\t}
}
"""
pristine = """void PristineMovie(Ctx& ctx, void* base) {
\t// bl 0x825d60c8
\tctx.lr = 0x825E05A0;
\trex_AptSafeQueueFixed_Enqueue_QAAXPAX_Z_825D60C8(ctx, base);
}
"""
harness = r"""
#include <cstdio>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
namespace rex::cvar {
std::map<std::string, bool> flags;
template<class T> T Query(std::string_view name) { return flags[std::string(name)]; }
void SetFlagByName(std::string_view name, std::string_view value) {
  flags[std::string(name)] = value == "true";
}
}
struct Ctx { struct { unsigned long long u64 = 123; } r3; unsigned lr = 0; };
unsigned original_calls = 0;
void rex_AptSafeQueueFixed_Enqueue_QAAXPAX_Z_825D60C8(Ctx&, void*) { ++original_calls; }
"""
harness += callback
main = r"""
int main() {
  unsigned cases = 0, failures = 0;
  for (bool automate : {false, true})
  for (bool probe : {false, true})
  for (bool keep_signed_in : {false, true})
  for (bool legacy_skip : {false, true}) {
    rex::cvar::flags = {
      {"skate3_demo_path", automate}, {"skate3_demo_path_probe", probe},
      {"skate3_demo_path_signed_in", keep_signed_in},
      {"skate3_intro_movie_skip", legacy_skip},
      {"user_profile_signed_in", true}, {"user_live_signed_in", true}
    };
    ApplyDemoPathProfileOverride();
    bool signed_in = !(automate && !keep_signed_in);
    if (rex::cvar::Query<bool>("user_profile_signed_in") != signed_in ||
        rex::cvar::Query<bool>("user_live_signed_in") != signed_in) ++failures;
    Ctx ctx;
    original_calls = 0;
    LegacyMovie(ctx, nullptr);
    PristineMovie(ctx, nullptr);
    if (original_calls != 2 || ctx.r3.u64 != 123) ++failures;
    ++cases;
  }
  std::printf("%u boot configurations checked; %u failures\n", cases, failures);
  return failures ? 1 : 0;
}
"""
with tempfile.TemporaryDirectory(prefix='skate3-boot-test-', dir=args.build_dir) as folder:
    folder = Path(folder).resolve()
    old = folder / 'legacy.cpp'
    fresh = folder / 'fresh.cpp'
    old.write_text(legacy)
    fresh.write_text(pristine)
    script = folder / 'migrate.cmake'
    helper = (args.root / 'cmake/RemoveLegacyMovieOverride.cmake').resolve().as_posix()
    script.write_text(f'include("{helper}")\n'
                      f'skate3_remove_legacy_movie_override("{old.as_posix()}")\n'
                      f'skate3_remove_legacy_movie_override("{fresh.as_posix()}")\n')
    subprocess.run(['cmake', '-P', str(script)], check=True)
    assert 'ShouldForceIntroMovieComplete' not in old.read_text()
    assert fresh.read_text() == pristine, 'Fresh generation must remain untouched'
    migrated = old.read_bytes()
    subprocess.run(['cmake', '-P', str(script)], check=True)
    assert old.read_bytes() == migrated, 'Migration must be idempotent'
    source = folder / 'boot_test.cpp'
    source.write_text(harness + old.read_text() + fresh.read_text() + main)
    executable = folder / 'boot_test.exe'
    if Path(args.cxx).name.lower() in ('clang-cl.exe', 'clang-cl', 'cl.exe', 'cl'):
        command = [args.cxx, '/nologo', '/std:c++20', '/EHsc',
                   '/Fe' + str(executable), '/Fo' + str(folder / 'boot_test.obj'), str(source)]
    else:
        command = [args.cxx, '-std=c++20', str(source), '-o', str(executable)]
    subprocess.run(command, check=True)
    subprocess.run([str(executable)], check=True)
