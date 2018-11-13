from test_common import Catch, AsRoot, AsAlice, ReloadPortod

import sys
import os
import time
import socket
import unittest

import porto


prefix = "test-api.py-"
container_name = prefix + "a"
layer_name = prefix + "layer"
volume_private = prefix + "volume"
volume_size = 256 * (2 ** 20)
volume_size_eps = 40 * (2 ** 20)  # loop/ext4: 5% reserve + 256 byte inode per 4k of data
volume_path = "/tmp/" + prefix + "layer"
tarball_path = "/tmp/" + prefix + "layer.tgz"
storage_tarball_path = "/tmp/" + prefix + "storage.tgz"
storage_name = prefix + "volume_storage"
meta_storage_name = prefix + "meta_storage"
storage_in_meta = meta_storage_name + "/storage"
layer_in_meta = meta_storage_name + "/layer"
blackhole = '/run/portod.socket.blackhole'


class TestApi(unittest.TestCase):
    def tearDown(self):
        AsRoot()

        c = porto.Connection()

        if not Catch(c.Find, container_name):
            c.Destroy(container_name)

        if not Catch(c.FindVolume, volume_path):
            c.DestroyVolume(volume_path)

        if os.access(volume_path, os.F_OK):
            os.rmdir(volume_path)

        for v in c.ListVolumes():
            if v.GetProperties().get("private") == volume_private:
                c.DestroyVolume(v.path)

        if not Catch(c.FindLayer, layer_name):
            c.RemoveLayer(layer_name)

        if os.access(tarball_path, os.F_OK):
            os.unlink(tarball_path)

        if os.access(storage_tarball_path, os.F_OK):
            os.unlink(storage_tarball_path)

        for l in c.ListLayers():
            if l.name == layer_in_meta:
                l.Remove()

        for st in c.ListStorages():
            if st.name == storage_in_meta:
                st.Remove()

        for ms in c.ListMetaStorages():
            if ms.name == meta_storage_name:
                ms.Remove()

        c.Disconnect()

    def test_connection(self):
        c = porto.Connection()
        self.assertFalse(c.Connected())

        c.Connect()
        self.assertTrue(c.Connected())

        c.Disconnect()
        self.assertFalse(c.Connected())

        # AUTO CONNECT

        c.Version()
        self.assertTrue(c.Connected())

        # AUTO RECONNECT

        ReloadPortod()
        self.assertTrue(c.Connected())
        c.Version()
        self.assertTrue(c.Connected())

        # NO AUTO RECONNECT

        c.Disconnect()
        c.SetAutoReconnect(False)

        with self.assertRaises(porto.exceptions.SocketError):
            c.Version()

        self.assertFalse(c.Connected())

    def test_connection_error(self):
        if os.path.exists(blackhole):
            os.remove(blackhole)

        c = porto.Connection(socket_path=blackhole, timeout=1)
        start = time.time()
        with self.assertRaises(porto.exceptions.SocketError):
            c.Connect()

        self.assertFalse(c.Connected())
        self.assertLess(time.time() - start, 0.1)

    def test_connection_timeout(self):
        blackhole_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        blackhole_sock.bind(blackhole)
        blackhole_sock.listen(1)

        c = porto.Connection(socket_path=blackhole, timeout=1)
        start = time.time()
        with self.assertRaises(porto.exceptions.SocketTimeout):
            c.Connect()
            c.Version()

        self.assertFalse(c.Connected())
        self.assertGreater(time.time() - start, 0.9)

        os.remove(blackhole)

    def test_commands(self):
        AsAlice()

        c = porto.Connection()
        self.assertFalse(c.Connected())

        c.Connect()
        self.assertTrue(c.Connected())

        c.List()
        c.Plist()
        c.Dlist()
        c.Vlist()
        c.Version()

        c.ContainerProperties()
        c.VolumeProperties()

        c.ListContainers()
        c.ListVolumes()
        c.ListLayers()
        c.ListStorages()
        c.ListMetaStorages()
        c.GetVolumes()
        c.FindLabel("TEST.test")

        c.ListStorage()

        c.GetData("/", 'cpu_usage')
        c.GetProperty("/", 'cpu_usage')
        c.GetInt("/", 'cpu_usage')

        c.GetProperty("/", 'controllers', 'cpu')
        c.GetProperty("/", ('controllers', 'cpu'))

        c.GetInt("/", 'controllers', 'cpu')
        c.GetInt("/", ('controllers', 'cpu'))

        r = c.Find("/")
        r.GetData('cpu_usage')
        r.GetProperty('cpu_usage')

        r.GetProperty("controllers", "cpu")
        r.GetProperty(("controllers", "cpu"))

        r.GetInt("cpu_usage")

        r.GetInt("controllers", "cpu")
        r.GetInt(("controllers", "cpu"))

        # CONTAINERS

        assert Catch(c.Find, container_name) == porto.exceptions.ContainerDoesNotExist
        assert container_name not in c.List()

        a = c.Create(container_name)
        assert a.name == container_name
        assert c.Find(container_name).name == container_name
        assert container_name in c.List()

        assert a["state"] == "stopped"
        assert a.GetData("state") == "stopped"

        a.SetProperty("command", "false")
        assert a.GetProperty("command") == False

        a.SetProperty("memory_limit", "2G")
        assert a.GetProperty("memory_limit") == "2147483648"

        a.SetInt("memory_limit", 1 << 30)
        assert a.GetInt("memory_limit") == 1073741824

        a.Set(command="/bin/true", private="test")
        assert a.Get(["command", "state", "private"]) == {"command": "/bin/true", "state": "stopped", "private": "test"}

        a.Start()
        assert a.Wait() == a.name
        assert a.GetData("state") == "dead"
        assert a.GetData("exit_status") == "0"

        assert c.Wait(['*']) == a.name

        a.Stop()
        assert a.GetData("state") == "stopped"

        a.SetProperty("command", "sleep 60")
        a.Start()
        assert a.GetData("state") == "running"

        a.Pause()
        assert a.GetData("state") == "paused"

        a.Resume()
        assert a.GetData("state") == "running"

        a.Kill(9)
        assert a.Wait() == a.name
        assert a.GetData("state") == "dead"
        assert a.GetData("exit_status") == "9"

        a.Stop()
        assert a.GetData("state") == "stopped"

        a.SetProperty("command", "echo test")

        a.Start()
        assert a.Wait() == a.name
        assert a.GetData("exit_status") == "0"
        assert a.GetData("stdout") == "test\n"
        c.Destroy(a)

        assert Catch(c.Find, container_name) == porto.exceptions.ContainerDoesNotExist
        assert container_name not in c.List()

        a = c.Run(container_name, command="sleep 5", private_value=volume_private)
        assert a["command"] == "sleep 5"
        assert a["private"] == volume_private
        a.Destroy()

    def test_layers(self):
        AsAlice()
        c = porto.Connection()

        c.ListVolumes()
        v = c.CreateVolume(private=volume_private)
        v.GetProperties()
        v.Tune()
        f = open(v.path + "/file", 'w')
        f.write("test")
        f.close()

        v.Export(tarball_path)
        l = c.ImportLayer(layer_name, tarball_path)
        self.assertEqual(l.name, layer_name)
        self.assertEqual(c.FindLayer(layer_name).name, layer_name)

        self.assertEqual(l.GetPrivate(), "")
        l.SetPrivate("123654")
        self.assertEqual(l.GetPrivate(), "123654")
        l.SetPrivate("AbC")
        self.assertEqual(l.GetPrivate(), "AbC")

        l.Update()
        self.assertEqual(l.owner_user, "porto-alice")
        self.assertEqual(l.owner_group, "porto-alice")
        self.assertGreaterEqual(l.last_usage, 0)
        self.assertEqual(l.private_value, "AbC")

        with self.assertRaises(porto.exceptions.LayerNotFound):
            c.GetLayerPrivate("my1980")
        with self.assertRaises(porto.exceptions.LayerNotFound):
            c.SetLayerPrivate("my1980", "my1980")

        with self.assertRaises(porto.exceptions.InvalidPath):
            c.CreateVolume(volume_path)

        os.mkdir(volume_path)
        w = c.CreateVolume(volume_path, layers=[layer_name])
        self.assertEqual(w.path, volume_path)
        self.assertEqual(c.FindVolume(volume_path).path, volume_path)
        self.assertEqual(len(w.GetLayers()), 1)
        self.assertEqual(w.GetLayers()[0].name, layer_name)
        with open(w.path + "/file", 'r+') as f:
            self.assertEqual(f.read(), "test")
        w.Unlink()

        w = c.CreateVolume(volume_path, layers=[layer_name], space_limit=str(volume_size))
        self.assertEqual(w.path, volume_path)
        self.assertEqual(c.FindVolume(volume_path).path, volume_path)
        self.assertEqual(len(w.GetLayers()), 1)
        self.assertEqual(w.GetLayers()[0].name, layer_name)

        with open(w.path + "/file", 'r+') as f:
            self.assertEqual(f.read(), "test")
            self.assertLessEqual(int(w.GetProperty("space_used")), volume_size_eps)
            self.assertGreater(int(w.GetProperty("space_available")), volume_size - volume_size_eps)

            f.write("x" * (volume_size - volume_size_eps * 2))

            self.assertGreaterEqual(int(w.GetProperty("space_used")), volume_size - volume_size_eps * 2)
            self.assertLess(int(w.GetProperty("space_available")), volume_size_eps * 2)
            with self.assertRaises(IOError):
                f.write("x" * volume_size_eps * 2)
            self.assertGreaterEqual(int(w.GetProperty("space_used")), volume_size - volume_size_eps)
            self.assertLess(int(w.GetProperty("space_available")), volume_size_eps)

        a = c.Create(container_name)
        w.Link(a)
        self.assertEqual(len(w.GetContainers()), 2)
        self.assertEqual(len(w.ListVolumeLinks()), 2)
        w.Unlink()
        self.assertEqual(len(w.GetContainers()), 1)
        self.assertEqual(len(w.ListVolumeLinks()), 1)
        self.assertEqual(w.GetContainers()[0].name, container_name)
        with self.assertRaises(porto.exceptions.Busy):
            l.Remove()

        v.Unlink()
        with self.assertRaises(porto.exceptions.VolumeNotFound):
            c.FindVolume(v.path)

        c.Destroy(a)
        with self.assertRaises(porto.exceptions.VolumeNotFound):
            c.FindVolume(w.path)

        v = c.CreateVolume()
        c.GetVolume(v.path)
        c.DestroyVolume(v.path)

        v = c.NewVolume({})
        c.GetVolume(v['path'])
        c.GetVolumes([v['path']])
        c.DestroyVolume(v['path'])

        l.Remove()
        os.rmdir(volume_path)

    def test_storage(self):
        AsAlice()
        c = porto.Connection()

        v = c.CreateVolume(storage=storage_name, private_value=volume_private)
        self.assertEqual(v.storage.name, storage_name)
        self.assertEqual(v.private, volume_private)
        self.assertEqual(v.private_value, volume_private)
        self.assertEqual(v["private"], volume_private)
        self.assertEqual(v.GetProperty("private"), volume_private)
        self.assertEqual(c.FindStorage(storage_name).name, storage_name)
        v.Destroy()

        v = c.CreateVolume(storage=storage_name)
        self.assertEqual(v.private_value, volume_private)
        v.Destroy()

        st = c.FindStorage(storage_name)
        self.assertEqual(st.private_value, volume_private)
        st.Export(storage_tarball_path)
        st.Remove()

        with self.assertRaises(porto.exceptions.VolumeNotFound):
            c.FindStorage(storage_name)

        c.ImportStorage(storage_name, storage_tarball_path, private_value=volume_private)
        st = c.FindStorage(storage_name)
        self.assertEqual(st.private_value, volume_private)
        st.Remove()
        os.unlink(storage_tarball_path)

    def test_meta_storage(self):
        AsAlice()
        c = porto.Connection()
        ms = c.CreateMetaStorage(meta_storage_name, space_limit=2**20)

        ml = c.ImportLayer(layer_in_meta, tarball_path)
        self.assertEqual(c.FindLayer(layer_in_meta).name, layer_in_meta)
        self.assertEqual(ms.FindLayer("layer").name, layer_in_meta)
        self.assertEqual(len(ms.ListLayers()), 1)

        with self.assertRaises(porto.exceptions.Busy):
            ms.Remove()

        ms.Update()
        self.assertEqual(ms.space_limit, 2**20)
        self.assertTrue(0 < ms.space_available < 2**20)
        self.assertTrue(0 < ms.space_used < 2**20)
        self.assertEqual(ms.space_used + ms.space_available, ms.space_limit)

        ms.Resize(space_limit=2**30)
        self.assertEqual(ms.space_limit, 2**30)

        v = c.CreateVolume(storage=storage_in_meta, private=volume_private, layers=[ml])
        st = ms.FindStorage("storage")
        self.assertEqual(st.name, storage_in_meta)
        self.assertEqual(c.FindStorage(storage_in_meta).name, storage_in_meta)
        self.assertEqual(len(ms.ListStorages()), 1)
        v.Destroy()

        ml.Remove()

        st.Export(storage_tarball_path)
        st.Remove()
        self.assertEqual(len(ms.ListStorages()), 0)
        st.Import(storage_tarball_path)
        self.assertEqual(len(ms.ListStorages()), 1)
        st.Remove()

        ms.Remove()

    def test_weak_containers(self):
        AsAlice()
        c = porto.Connection()
        a = c.CreateWeakContainer(container_name)
        a.SetProperty("command", "sleep 60")
        a.Start()
        c.Disconnect()
        c.Connect()
        if Catch(c.Find, container_name) != porto.exceptions.ContainerDoesNotExist:
            Catch(c.Wait, [container_name], 1000)
            self.assertEqual(Catch(c.Destroy, container_name), porto.exceptions.ContainerDoesNotExist)

        Catch(c.Destroy, container_name)

    def test_pid_and_reconnect(self):
        AsAlice()
        c = porto.Connection()
        c.Connect()

        c2 = porto.Connection(auto_reconnect=False)
        c2.Connect()

        pid = os.fork()
        if pid:
            _, status = os.waitpid(pid, 0)
            self.assertEqual(status, 0)
        else:
            c.Version()
            with self.assertRaises(porto.exceptions.SocketError):
                c2.Version()
            sys.exit(0)

        c2.Disconnect()
        c.Disconnect()


if __name__ == '__main__':
    unittest.main()
