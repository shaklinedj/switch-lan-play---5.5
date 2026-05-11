FROM node:18.20.5-alpine

RUN mkdir /app
WORKDIR /app
COPY ./package.json /app/package.json
RUN npm install
COPY . /app
RUN npm run build

CMD [ "npm", "run", "server" ]
