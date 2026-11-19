"""Tests del hook global de secretos (herramientas/hook_secretos/).

Las reglas se prueban sobre líneas sueltas; los hooks, con repositorios git temporales que
usan como core.hooksPath la carpeta del repo, igual que en la PC de un alumno.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

AQUI = os.path.dirname(os.path.abspath(__file__))
HOOKS = os.path.join(AQUI, "..", "hook_secretos")
sys.path.insert(0, HOOKS)

from revisar_secretos import archivo_prohibido, revisar_linea  # noqa: E402

# Valores inventados para las pruebas: no son claves de ningún lado. Los nombres de las variables
# se arman por partes ("PA" + "SS") para que ni gitleaks ni este mismo hook marquen el archivo.
CLAVE = "pRu3ba-" + "inventada"
PASS, CLV, PWD = "PA" + "SS", "CLA" + "VE", "pass" + "word"
DEFINE_WIFI = f'#define WIFI_{PASS} "{CLAVE}"'


class Reglas(unittest.TestCase):
    def assertDetecta(self, linea):
        self.assertTrue(revisar_linea(linea), f"debería detectar: {linea}")

    def assertPasa(self, linea):
        self.assertEqual(revisar_linea(linea), [], f"no debería detectar: {linea}")

    def test_define_de_claves(self):
        self.assertDetecta(DEFINE_WIFI)
        self.assertDetecta(f'#define MQTT_{PASS}      "{CLAVE}"')
        self.assertDetecta(f'#define MESH_AP_{PASS} "{CLAVE}"')
        self.assertDetecta(f'#define ROUTER_{CLV} "{CLAVE}"')

    def test_asignaciones(self):
        self.assertDetecta(f'const char* {PWD} = "{CLAVE}";')
        self.assertDetecta(f'mqtt_{PASS.lower()}: {CLAVE}')
        self.assertDetecta(f'wifi_{CLV.lower()} = "{CLAVE}"')

    def test_mesh_id_y_claves_aes(self):
        self.assertDetecta("#define MESH_" + "ID_BYTES " + ", ".join(["0x12", "0xA4", "0x00", "0x7F", "0x01", "0x99"]))
        self.assertDetecta("static const uint8_t app" + "_key[16] = {" + ", ".join(["0x1F"] * 16) + "};")
        self.assertDetecta("app" + '_key = "' + "0011223344556677" * 2 + '"')

    def test_clave_privada(self):
        self.assertDetecta("-----BEGIN RSA " + "PRIVATE KEY-----")
        self.assertDetecta("-----BEGIN " + "PRIVATE KEY-----")

    def test_marcadores_de_posicion_pasan(self):
        self.assertPasa(f'#define WIFI_{PASS} "CAMBIAR"')
        self.assertPasa(f'#define MESH_AP_{PASS}    "CAMBIAR"   /* mínimo 8 caracteres */')
        self.assertPasa(f'#define MESH_AP_{PASS}    "CAMBIAR12"   /* mínimo 8 caracteres */')
        self.assertPasa(f'#define MQTT_{CLV}      ""')
        self.assertPasa('{ "AURA-BANCO", "<clave del WiFi>" }')
        self.assertPasa("#define MESH_" + "ID_BYTES   " + ", ".join(["0x00"] * 6) + "     /* 6 bytes */")
        self.assertPasa("static const uint8_t app" + "_key[16] = {" + ", ".join(["0x00"] * 16) + "};")
        self.assertPasa(f"{PWD} = ${{MQTT_PASSWORD}}")

    def test_codigo_que_nombra_claves_pasa(self):
        self.assertPasa(f"{PWD} = args.clave")
        self.assertPasa(f"{PWD} = input('Clave: ')")
        self.assertPasa(f"cfg.mesh_clave = MESH_AP_{PASS};")
        self.assertPasa(f"(BROKER_URI[0] && MQTT_{CLV}[0]) ? MQTT_{CLV} : NULL")
        self.assertPasa("Lleva el ID y la clave de la mesh de `config_local.h`.")

    def test_el_hallazgo_no_muestra_la_clave(self):
        [h] = revisar_linea(DEFINE_WIFI)
        self.assertNotIn(CLAVE, h.linea_tapada)
        self.assertIn("WIFI_", h.linea_tapada)


class ArchivosProhibidos(unittest.TestCase):
    def test_prohibidos(self):
        for ruta in ["config_local.h", "firmware/control-luz/main/config_local.h",
                     "Mesh3_Gateway/main/credenciales.h", ".env", "app/.env",
                     "firma_prueba.pem", "claves/firma.pem", "claves/notas.txt", "ssh/id.key"]:
            self.assertTrue(archivo_prohibido(ruta), ruta)

    def test_permitidos(self):
        for ruta in ["main/config_local.ejemplo.h", "config_local.h.example", ".env.example",
                     "README.md", "comun/claves_aes.h", "docs/clave-de-firma.md"]:
            self.assertFalse(archivo_prohibido(ruta), ruta)


class Hooks(unittest.TestCase):
    """Un repo con el hook global instalado, como en la PC de un alumno."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)
        cfg = os.path.join(self.tmp, "gitconfig")
        with open(cfg, "w") as f:
            f.write("[user]\n\tname = Alumno\n\temail = alumno@example.com\n"
                    f"[core]\n\thooksPath = {os.path.abspath(HOOKS)}\n"
                    "[init]\n\tdefaultBranch = main\n")
        self.env = dict(os.environ, GIT_CONFIG_GLOBAL=cfg, GIT_CONFIG_NOSYSTEM="1")
        self.remoto = os.path.join(self.tmp, "remoto.git")
        self.repo = os.path.join(self.tmp, "repo")
        self.git("init", "-q", "--bare", self.remoto, cwd=self.tmp)
        self.git("init", "-q", self.repo, cwd=self.tmp)
        self.git("remote", "add", "origin", self.remoto)
        self.escribir("README.md", "proyecto\n")
        self.git("add", ".")
        self.git("commit", "-q", "-m", "inicio")

    def git(self, *args, cwd=None, ok=True):
        r = subprocess.run(["git", *args], cwd=cwd or self.repo, env=self.env,
                           capture_output=True, text=True)
        if ok and r.returncode != 0:
            self.fail(f"git {' '.join(args)} falló:\n{r.stdout}{r.stderr}")
        return r

    def escribir(self, ruta, texto):
        ruta = os.path.join(self.repo, ruta)
        os.makedirs(os.path.dirname(ruta), exist_ok=True)
        with open(ruta, "w") as f:
            f.write(texto)

    def commit(self, *extra):
        self.git("add", "-A")
        return self.git("commit", "-q", "-m", "cambio", *extra, ok=False)

    def test_bloquea_config_local(self):
        self.escribir("firmware/main/config_local.h", '#define WIFI_SSID "AULA"\n')
        r = self.commit()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("firmware/main/config_local.h", r.stderr)

    def test_bloquea_clave_en_codigo_sin_mostrarla(self):
        self.escribir("main/app.c", f'int x;\n{DEFINE_WIFI}\n')
        r = self.commit()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("main/app.c:2", r.stderr)
        self.assertNotIn(CLAVE, r.stderr + r.stdout)

    def test_deja_pasar_la_plantilla(self):
        self.escribir("main/config_local.ejemplo.h", f'#define WIFI_{PASS} "CAMBIAR"\n')
        self.assertEqual(self.commit().returncode, 0)

    def test_solo_revisa_lo_que_se_agrega(self):
        self.escribir("main/app.c", f'{DEFINE_WIFI}\n')
        self.commit("--no-verify")
        self.escribir("main/otro.c", "int y;\n")
        self.assertEqual(self.commit().returncode, 0)

    def test_corre_el_hook_propio_del_repo(self):
        marca = os.path.join(self.tmp, "corrio")
        hook = os.path.join(self.repo, ".git", "hooks", "pre-commit")
        with open(hook, "w") as f:
            f.write(f"#!/bin/sh\ntouch '{marca}'\n")
        os.chmod(hook, 0o755)
        self.escribir("main/otro.c", "int y;\n")
        self.assertEqual(self.commit().returncode, 0)
        self.assertTrue(os.path.exists(marca))

    def test_el_hook_propio_que_falla_bloquea(self):
        hook = os.path.join(self.repo, ".git", "hooks", "pre-commit")
        with open(hook, "w") as f:
            f.write("#!/bin/sh\nexit 1\n")
        os.chmod(hook, 0o755)
        self.escribir("main/otro.c", "int y;\n")
        self.assertNotEqual(self.commit().returncode, 0)

    def test_push_ataja_un_revert_que_vuelve_a_subir_la_clave(self):
        # El caso del 2026-11-16: se sube la clave, se borra, y un revert la vuelve a poner.
        # git revert no corre el pre-commit: lo tiene que atajar el pre-push.
        self.escribir("main/app.c", f'{DEFINE_WIFI}\n')
        self.commit("--no-verify")
        self.escribir("main/app.c", f'#define WIFI_{PASS} ""\n')
        self.commit("--no-verify")
        self.git("push", "-q", "--no-verify", "origin", "main")
        self.git("revert", "--no-edit", "HEAD")
        r = self.git("push", "origin", "main", ok=False)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("main/app.c:1", r.stderr)
        self.assertNotIn(CLAVE, r.stderr + r.stdout)

    def test_push_de_rama_nueva_revisa_sus_commits(self):
        self.git("push", "-q", "origin", "main")
        self.git("switch", "-q", "-c", "rama")
        self.escribir("claves/firma_prueba.pem", "no es una clave\n")
        self.commit("--no-verify")
        r = self.git("push", "origin", "rama", ok=False)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("claves/firma_prueba.pem", r.stderr)

    def test_push_force_sobre_commits_que_no_estan_en_la_pc(self):
        # Otro clon sube un commit; esta PC no lo baja y fuerza el push con una clave.
        otro = os.path.join(self.tmp, "otro")
        self.git("push", "-q", "origin", "main")
        self.git("clone", "-q", self.remoto, otro, cwd=self.tmp)
        with open(os.path.join(otro, "nuevo.txt"), "w") as f:
            f.write("de otro\n")
        self.git("add", ".", cwd=otro)
        self.git("commit", "-q", "-m", "otro", cwd=otro)
        self.git("push", "-q", "origin", "main", cwd=otro)
        self.escribir("main/app.c", f"{DEFINE_WIFI}\n")
        self.commit("--no-verify")
        r = self.git("push", "--force", "origin", "main", ok=False)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("main/app.c:1", r.stderr)
        self.assertNotIn("Traceback", r.stderr)

    def test_push_limpio_pasa(self):
        self.escribir("main/otro.c", "int y;\n")
        self.commit()
        self.assertEqual(self.git("push", "origin", "main", ok=False).returncode, 0)


if __name__ == "__main__":
    unittest.main()
