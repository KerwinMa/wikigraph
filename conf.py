
# Located in dumps folder, specify only first common part
DUMPFILES = "enwiki-20110115-pages-articles"

# Write log
LOGFILE = 'log.txt'

# Select debug mode
DEBUG = 1

# Which namespaces to ignore, all lowercase
NAMESPACES = [ u'file', u'image',
    u's', u'user', u'template', u'wiktionary', u'wikt', 
    u'wikipedia', u'wp', u'user_talk', u'help', u'wikisource',
    u'wikibooks', u'portal', u'commons',u'talk',u'media',
    u'wikipedia talk']

# Database
REDIS_HOST = 'localhost'
REDIS_PORT = 6379
REDIS_DB = 0

