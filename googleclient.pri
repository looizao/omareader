# Writes googleclient.h into the build directory from GOOGLE_CLIENT_ID and,
# when present, GOOGLE_CLIENT_SECRET; shared by the app and the tests.
GOOGLE_ID = $$cat($$PWD/GOOGLE_CLIENT_ID)
GOOGLE_SECRET =
exists($$PWD/GOOGLE_CLIENT_SECRET): GOOGLE_SECRET = $$cat($$PWD/GOOGLE_CLIENT_SECRET)
GOOGLE_HEADER = "$${LITERAL_HASH}pragma once"
GOOGLE_HEADER += "$${LITERAL_HASH}define OMAREADER_GOOGLE_CLIENT_ID \"$$GOOGLE_ID\""
GOOGLE_HEADER += "$${LITERAL_HASH}define OMAREADER_GOOGLE_SECRET \"$$GOOGLE_SECRET\""
write_file($$OUT_PWD/googleclient.h, GOOGLE_HEADER)
INCLUDEPATH += $$OUT_PWD
