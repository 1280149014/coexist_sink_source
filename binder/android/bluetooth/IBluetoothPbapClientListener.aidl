/*
 * Copyright 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package android.bluetooth;

import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothContact;
import android.bluetooth.BluetoothCallLog;

/**
 * API for Bluetooth Phone Book Access Provile Client Side Listener
 *
 * {@hide}
 */
oneway interface IBluetoothPbapClientListener {
    void onSyncStateChanged(in BluetoothDevice bluetoothDevice, in int currentSyncState ,
            in int  preSyncState, in boolean isType);
    void onContactFetched(in BluetoothDevice bluetoothDevice, in BluetoothContact bluetoothContact);
    void onContactItemCountDetermined(in BluetoothDevice bluetoothDevice,
            in int count);
    void onCallLogFetched(in BluetoothDevice bluetoothDevice, in BluetoothCallLog bluetoothCallLog);
    void onCallLogItemCountDetermined(in BluetoothDevice bluetoothDevice,
            in int count);
    void onDatabaseSyncComplete(in BluetoothDevice bluetoothDevice, in boolean isType);
}
