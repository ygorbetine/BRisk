#!/usr/bin/env python3
"""Redefine a senha de uma conta local do Briks (feche o Briks antes).

Uso: python scripts/reset-local-password.py [--visible] [caminho do risk.sqlite3]
--visible mostra a senha enquanto digita (use se o terminal não aceitar o campo oculto).
Pede o e-mail e a nova senha (sem mostrar na tela). A senha nunca é gravada em texto:
o banco guarda só o hash argon2id (mesmos parâmetros do backend).
"""
import base64, getpass, os, sqlite3, subprocess, sys

DEFAULT_DB = os.path.join(os.environ.get("APPDATA", ""), "@briks", "desktop", "risk.sqlite3")
M_COST, T_COST, LANES, KEYLEN = 19456, 2, 1, 32


def b64(data: bytes) -> str:
    return base64.b64encode(data).decode().rstrip("=")


def argon2id_phc(password: str, salt: bytes) -> str:
    out = subprocess.run(
        ["openssl", "kdf", "-keylen", str(KEYLEN), "-kdfopt", "hexpass:" + password.encode().hex(),
         "-kdfopt", "hexsalt:" + salt.hex(), "-kdfopt", f"iter:{T_COST}", "-kdfopt", f"memcost:{M_COST}",
         "-kdfopt", f"lanes:{LANES}", "-kdfopt", f"threads:{LANES}", "-binary", "ARGON2ID"],
        capture_output=True, check=True).stdout
    return f"$argon2id$v=19$m={M_COST},t={T_COST},p={LANES}${b64(salt)}${b64(out)}"


def ask_secret(prompt: str, visible: bool) -> str:
    return input(prompt) if visible else getpass.getpass(prompt)


def main() -> int:
    args = [a for a in sys.argv[1:] if a != "--visible"]
    visible = "--visible" in sys.argv[1:]
    db_path = args[0] if args else DEFAULT_DB
    if not os.path.exists(db_path):
        print("Banco não encontrado:", db_path); return 1
    db = sqlite3.connect(db_path, timeout=5)
    users = db.execute("SELECT email, display_name FROM users ORDER BY created_at").fetchall()
    print("Contas neste computador:")
    for email, name in users:
        print(f"  {email}  ({name})")
    email = os.environ.get("BRIKS_RESET_EMAIL") or input("E-mail da conta: ").strip()
    password = os.environ.get("BRIKS_RESET_PASSWORD") or ask_secret("Nova senha (mínimo 8 caracteres): ", visible)
    if len(password) < 8:
        print("A senha precisa ter pelo menos 8 caracteres."); return 1
    if not os.environ.get("BRIKS_RESET_PASSWORD") and ask_secret("Repita a nova senha: ", visible) != password:
        print("As senhas não conferem."); return 1
    phc = argon2id_phc(password, os.urandom(16))
    cursor = db.execute("UPDATE users SET password_hash=? WHERE email=? COLLATE NOCASE", (phc, email))
    db.commit()
    if cursor.rowcount != 1:
        print("Nenhuma conta com esse e-mail."); return 1
    print("Senha redefinida. Abra o Briks e entre com a nova senha.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
