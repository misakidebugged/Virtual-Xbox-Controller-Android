package com.virtualcontroller.android.ipc;

interface IInputEventListener {
    void onInputFrame(in byte[] report);
}
