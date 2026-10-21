package com.steamcontroller.android.ipc;

interface IInputEventListener {
    void onInputFrame(in byte[] report);
}
