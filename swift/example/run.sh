#!/usr/bin/env bash
# Copyright (c) 2025 OceanBase.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}
simulator=${1:?Usage: swift/example/run.sh SIMULATOR_UDID}
bundle=org.seekdb.example
derived="$here/build"

if [[ ! -d "$root/swift/build/SeekDBEngine.xcframework" ]]; then
  echo "Run SEEKDB_DIR=<seekdb checkout> bash swift/build.sh first" >&2
  exit 1
fi
xcodegen generate --spec "$here/project.yml" --project "$here" --quiet
xcodebuild -project "$here/SeekDBExample.xcodeproj" -scheme SeekDBExample \
  -destination "platform=iOS Simulator,id=$simulator" -derivedDataPath "$derived" build
xcrun simctl install "$simulator" "$derived/Build/Products/Debug-iphonesimulator/SeekDBExample.app"
report="$(xcrun simctl get_app_container "$simulator" "$bundle" data)/Documents/select1.json"
rm -f "$report"
xcrun simctl launch --terminate-running-process "$simulator" "$bundle" > /dev/null
for _ in $(seq 300); do
  if [[ -s "$report" ]]; then
    cat "$report"
    echo
    grep -q '"select_1":1' "$report"
    exit
  fi
  sleep 1
done
echo "No result in $report after 300 seconds" >&2
exit 1
