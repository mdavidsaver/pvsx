import os

from epicscorelibs import ioc
import pvxslibs.path

if __name__ == "__main__":
    os.environ.setdefault("PVXS_QSRV_ENABLE", "YES")
    pvxs_dbd_load = (("pvxsIoc.dbd", pvxslibs.path.dbd_path), )
    pvxs_dso_load = ("pvxslibs.lib.pvxsIoc", )
    ioc.main(extra_dbd_load=pvxs_dbd_load, extra_dso_load=pvxs_dso_load)
