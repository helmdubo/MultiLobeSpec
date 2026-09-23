#!/bin/bash
# usage: build.sh <round>   stages the worktree plugin and runs RunUAT BuildPlugin -StrictIncludes
set +e
R=${1:-1}; B="$(cd "$(dirname "$0")" && pwd)"; W="E:/GITHUB/MultiLobeSpec/MultiLobeSpec/.claude/worktrees/volumetric-fog-multiple-scattering-audit-494905"
STAGE="$B/Source$R/MultiLobeSpec"; PKG="$B/Package$R"; LOG="$B/Build$R.log"
[ -e "$STAGE" ] && { echo "round $R exists"; exit 2; }
mkdir -p "$STAGE"
for n in Source Shaders Content Config Resources; do [ -d "$W/$n" ] && cp -r "$W/$n" "$STAGE/$n"; done
cp "$W"/*.uplugin "$STAGE/"; cp "$W"/*.md "$STAGE/" 2>/dev/null || true
cmd //c "D:\PersonalProjects\UE5\UE_5.8\Engine\Build\BatchFiles\RunUAT.bat BuildPlugin -Plugin=$(cygpath -w "$STAGE")\MultiLobeSpec.uplugin -Package=$(cygpath -w "$PKG") -TargetPlatforms=Win64 -StrictIncludes" > "$LOG" 2>&1; code=$?
tail -5 "$LOG"
for f in MultiLobeSpec.uplugin Binaries/Win64/UnrealEditor-MultiLobeSpec.dll Binaries/Win64/UnrealEditor-FogMSRender.dll Binaries/Win64/UnrealEditor-MultiLobeSpecEditor.dll; do [ -f "$PKG/$f" ] || { echo "MISSING $f"; exit 1; }; done
echo "BUILD PASS round $R package=$PKG"
