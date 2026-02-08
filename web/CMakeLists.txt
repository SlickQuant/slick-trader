# SlickTrader - Web Frontend

# Install web static files
install(DIRECTORY static/
    DESTINATION share/slick-trader/web
    OPTIONAL
    FILES_MATCHING
    PATTERN "*.html"
    PATTERN "*.css"
    PATTERN "*.js"
    PATTERN "*.png"
    PATTERN "*.jpg"
    PATTERN "*.svg"
)
