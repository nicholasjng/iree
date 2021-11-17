// RUN: iree-opt -split-input-file -iree-hal-conversion %s | IreeFileCheck %s

// TODO(#7277): swap HAL Conversion->Conversion and add tests.
// CHECK: @todo
func @todo() {
  return
}
